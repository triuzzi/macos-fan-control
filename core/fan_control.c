#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <math.h>
#include <IOKit/IOKitLib.h>

#define SELECTOR 2
#define CMD_READ 5
#define CMD_WRITE 6
#define CMD_INDEX 8
#define CMD_INFO 9
#define SETTLE_MICROSECONDS 400000
#define TOLERANCE 25.0
#define MAX_FANS 8
#define DEFAULT_CEILING 95.0
#define DEFAULT_BELOW 50.0
#define DEFAULT_TRIGGER_ON 80.0
#define DEFAULT_TRIGGER_BELOW 60.0
#define COOL_POLL_MICROSECONDS 1000000
#define SENSOR_HOTTEST 0
#define SENSOR_CPU 1
#define SENSOR_GPU 2
#ifndef COOL_STATE
#define COOL_STATE "/var/run/fan_control.cool"
#endif
#ifndef TRIGGER_STATE
#define TRIGGER_STATE "/var/run/fan_control.trigger"
#endif

typedef struct {
  uint32_t key;
  uint8_t versionAndLimits[24];
  uint32_t size;
  uint32_t type;
  uint8_t attributes;
  uint8_t pad0[3];
  char result;
  char status;
  char command;
  uint8_t pad1;
  uint32_t index;
  uint8_t bytes[32];
} Message;

_Static_assert(sizeof(Message) == 80, "SMC message layout");
_Static_assert(__builtin_offsetof(Message, size) == 28, "size offset");
_Static_assert(__builtin_offsetof(Message, result) == 40, "result offset");
_Static_assert(__builtin_offsetof(Message, index) == 44, "index offset");
_Static_assert(__builtin_offsetof(Message, bytes) == 48, "bytes offset");

typedef enum { WATCH_COOL, WATCH_TRIGGER } WatchKind;

typedef struct {
  WatchKind kind;
  int pid;
  double percent, below, ceiling;
  double on;
  int sensor;
  int holding;
} Watch;

static io_connect_t connection;
static int readWatch(WatchKind kind, Watch *watch);
static void stopWatchers(void);
static const char *sensorName(int sensor);
static int groupPresent(int sensor, int cpuCount, int gpuCount);
static double reading(int sensor, double cpu, double gpu);

static int openSMC(void) {
  io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
  if (!service) { fprintf(stderr, "fan_control: no AppleSMC\n"); return 1; }
  int failed = IOServiceOpen(service, mach_task_self(), 0, &connection) != kIOReturnSuccess;
  IOObjectRelease(service);
  if (failed) { fprintf(stderr, "fan_control: cannot open AppleSMC\n"); return 1; }
  return 0;
}

static uint32_t encode(const char *name) {
  return (uint32_t)name[0] << 24 | (uint32_t)name[1] << 16 | (uint32_t)name[2] << 8 | (uint32_t)name[3];
}

static void decode(uint32_t key, char *out) {
  out[0] = key >> 24; out[1] = key >> 16; out[2] = key >> 8; out[3] = key; out[4] = 0;
}

static int call(Message *in, Message *out) {
  size_t size = sizeof(Message);
  if (IOConnectCallStructMethod(connection, SELECTOR, in, sizeof(Message), out, &size) != kIOReturnSuccess) return -1;
  return out->result ? (out->result & 0xFF) : 0;
}

static int readKey(const char *name, uint32_t *size, uint32_t *type, uint8_t *out) {
  Message in = {0}, reply = {0};
  in.key = encode(name);
  in.command = CMD_INFO;
  if (call(&in, &reply)) return -1;
  *size = reply.size;
  *type = reply.type;

  memset(&in, 0, sizeof in);
  memset(&reply, 0, sizeof reply);
  in.key = encode(name);
  in.command = CMD_READ;
  in.size = *size;
  if (call(&in, &reply)) return -1;
  memcpy(out, reply.bytes, *size);
  return 0;
}

static int writeKey(const char *name, uint32_t size, const uint8_t *bytes) {
  Message in = {0}, reply = {0};
  in.key = encode(name);
  in.command = CMD_WRITE;
  in.size = size;
  memcpy(in.bytes, bytes, size);
  return call(&in, &reply);
}

static double value(uint32_t type, uint32_t size, const uint8_t *bytes) {
  char text[5];
  decode(type, text);
  if (!strcmp(text, "flt ") && size == 4) { float f; memcpy(&f, bytes, 4); return f; }
  if (size == 1) return bytes[0];
  if (size == 4) return (double)((uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3]);
  return -1;
}

static int readNumber(const char *name, double *out) {
  uint32_t size, type;
  uint8_t bytes[32];
  if (readKey(name, &size, &type, bytes)) return -1;
  *out = value(type, size, bytes);
  return 0;
}

typedef struct { double actual, minimum, maximum, target; int forced; } Fan;

static int readFan(int index, Fan *fan) {
  char name[5];
  double mode;
  snprintf(name, sizeof name, "F%dAc", index); if (readNumber(name, &fan->actual)) return -1;
  snprintf(name, sizeof name, "F%dMn", index); if (readNumber(name, &fan->minimum)) return -1;
  snprintf(name, sizeof name, "F%dMx", index); if (readNumber(name, &fan->maximum)) return -1;
  snprintf(name, sizeof name, "F%dTg", index); if (readNumber(name, &fan->target)) return -1;
  snprintf(name, sizeof name, "F%dmd", index); if (readNumber(name, &mode)) return -1;
  fan->forced = mode >= 0.5;
  return 0;
}

static double percent(const Fan *fan, double rpm) {
  double span = fan->maximum - fan->minimum;
  if (span <= 0) return 0;
  double result = (rpm - fan->minimum) / span * 100;
  return result < 0 ? 0 : result > 100 ? 100 : result;
}

static int temperatures(double *cpu, int *cpuCount, double *gpu, int *gpuCount) {
  double total;
  if (readNumber("#KEY", &total)) return -1;
  double cpuSum = 0, gpuSum = 0;
  *cpuCount = *gpuCount = 0;

  for (uint32_t i = 0; i < (uint32_t)total; i++) {
    Message in = {0}, reply = {0};
    in.command = CMD_INDEX;
    in.index = i;
    if (call(&in, &reply)) continue;

    char name[5];
    decode(reply.key, name);
    int isCpu = !strncmp(name, "Tp", 2), isGpu = !strncmp(name, "Tg", 2);
    if (!isCpu && !isGpu) continue;

    uint32_t size, type;
    uint8_t bytes[32];
    char text[5];
    if (readKey(name, &size, &type, bytes)) continue;
    decode(type, text);
    if (strcmp(text, "flt ")) continue;

    double celsius = value(type, size, bytes);
    if (celsius <= 20 || celsius >= 130) continue;
    if (isCpu) { cpuSum += celsius; (*cpuCount)++; } else { gpuSum += celsius; (*gpuCount)++; }
  }
  *cpu = *cpuCount ? cpuSum / *cpuCount : 0;
  *gpu = *gpuCount ? gpuSum / *gpuCount : 0;
  return 0;
}

static int status(int count, int json, int withTemperatures) {
  Fan fans[MAX_FANS];
  for (int i = 0; i < count; i++)
    if (readFan(i, &fans[i])) { fprintf(stderr, "fan_control: cannot read fan %d\n", i); return 1; }

  double cpu = 0, gpu = 0;
  int cpuCount = 0, gpuCount = 0;
  if (withTemperatures && temperatures(&cpu, &cpuCount, &gpu, &gpuCount))
    { fprintf(stderr, "fan_control: cannot read sensors\n"); return 1; }

  Watch cooling, trigger;
  int coolingActive = readWatch(WATCH_COOL, &cooling);
  int triggering = readWatch(WATCH_TRIGGER, &trigger);

  if (!json) {
    for (int i = 0; i < count; i++)
      printf("fan %d  %.0f rpm  %.0f%%  target %.0f  %s\n", i, fans[i].actual,
             percent(&fans[i], fans[i].actual), fans[i].target, fans[i].forced ? "forced" : "auto");
    if (withTemperatures) printf("cpu %.1fC  gpu %.1fC\n", cpu, gpu);
    if (coolingActive)
      printf("cooling %.0f%% until below %.0fC (pid %d)\n", cooling.percent, cooling.below, cooling.pid);
    if (triggering)
      printf("trigger %s at %.0fC: hold %.0f%% until below %.0fC (%s, pid %d)\n",
             trigger.holding ? "holding" : "armed", trigger.on, trigger.percent,
             trigger.below, sensorName(trigger.sensor), trigger.pid);
    return 0;
  }

  printf("{\"fanCount\":%d,\"fans\":[", count);
  for (int i = 0; i < count; i++)
    printf("%s{\"index\":%d,\"actual\":%.0f,\"minimum\":%.0f,\"maximum\":%.0f,\"target\":%.0f,\"percent\":%.1f,\"forced\":%s}",
           i ? "," : "", i, fans[i].actual, fans[i].minimum, fans[i].maximum, fans[i].target,
           percent(&fans[i], fans[i].actual), fans[i].forced ? "true" : "false");
  printf("]");
  if (coolingActive)
    printf(",\"cooling\":{\"percent\":%.0f,\"below\":%.0f,\"pid\":%d}",
           cooling.percent, cooling.below, cooling.pid);
  if (triggering)
    printf(",\"trigger\":{\"percent\":%.0f,\"on\":%.0f,\"below\":%.0f,\"sensor\":\"%s\",\"state\":\"%s\",\"pid\":%d}",
           trigger.percent, trigger.on, trigger.below, sensorName(trigger.sensor),
           trigger.holding ? "holding" : "armed", trigger.pid);
  if (withTemperatures)
    printf(",\"temperatures\":{\"cpu\":%.1f,\"cpuSensors\":%d,\"gpu\":%.1f,\"gpuSensors\":%d}", cpu, cpuCount, gpu, gpuCount);
  printf("}\n");
  return 0;
}

static int force(int count, double pct) {
  if (pct < 0 || pct > 100) { fprintf(stderr, "fan_control: percent must be 0-100\n"); return 2; }
  float wanted[MAX_FANS];

  for (int i = 0; i < count; i++) {
    Fan fan;
    if (readFan(i, &fan)) { fprintf(stderr, "fan_control: cannot read fan %d\n", i); return 1; }
    wanted[i] = fan.minimum + (fan.maximum - fan.minimum) * pct / 100;

    char target[5], mode[5];
    uint8_t bytes[4], one = 1;
    snprintf(target, sizeof target, "F%dTg", i);
    snprintf(mode, sizeof mode, "F%dmd", i);
    memcpy(bytes, &wanted[i], 4);
    if (writeKey(target, 4, bytes) || writeKey(mode, 1, &one))
      { fprintf(stderr, "fan_control: write failed on fan %d (needs root)\n", i); return 1; }
  }

  usleep(SETTLE_MICROSECONDS);

  for (int i = 0; i < count; i++) {
    Fan fan;
    if (readFan(i, &fan)) return 1;
    if (!fan.forced || fabs(fan.target - wanted[i]) > TOLERANCE)
      { fprintf(stderr, "fan_control: fan %d rejected the request\n", i); return 1; }
    printf("fan %d forced to %.0f rpm (%.0f%%)\n", i, fan.target, pct);
  }
  return 0;
}

static int automatic(int count) {
  for (int i = 0; i < count; i++) {
    char mode[5];
    uint8_t zero = 0;
    snprintf(mode, sizeof mode, "F%dmd", i);
    if (writeKey(mode, 1, &zero)) { fprintf(stderr, "fan_control: write failed on fan %d (needs root)\n", i); return 1; }
  }

  usleep(SETTLE_MICROSECONDS);

  for (int i = 0; i < count; i++) {
    Fan fan;
    if (readFan(i, &fan)) return 1;
    if (fan.forced) { fprintf(stderr, "fan_control: fan %d still forced\n", i); return 1; }
    printf("fan %d automatic\n", i);
  }
  return 0;
}


static int guard(int count, double ceiling) {
  double cpu, gpu;
  int cpuCount, gpuCount;
  if (temperatures(&cpu, &cpuCount, &gpu, &gpuCount)) { fprintf(stderr, "fan_control: cannot read sensors\n"); return 1; }
  double hottest = cpu > gpu ? cpu : gpu;

  int forced = 0;
  for (int i = 0; i < count; i++) {
    Fan fan;
    if (readFan(i, &fan)) return 1;
    if (fan.forced) forced = 1;
  }

  if (!forced || hottest <= ceiling) { printf("ok %.1fC\n", hottest); return 0; }
  printf("guard tripped at %.1fC (ceiling %.0fC)\n", hottest, ceiling);
  stopWatchers();
  return automatic(count);
}

static const char *watchPath(WatchKind kind) {
  return kind == WATCH_COOL ? COOL_STATE : TRIGGER_STATE;
}

static int readWatch(WatchKind kind, Watch *watch) {
  FILE *file = fopen(watchPath(kind), "r");
  if (!file) return 0;
  watch->kind = kind;
  int parsed;
  if (kind == WATCH_COOL)
    parsed = fscanf(file, "%d %lf %lf %lf", &watch->pid, &watch->percent, &watch->below, &watch->ceiling) == 4;
  else
    parsed = fscanf(file, "%d %lf %lf %lf %lf %d %d", &watch->pid, &watch->on, &watch->percent,
                    &watch->below, &watch->ceiling, &watch->sensor, &watch->holding) == 7;
  fclose(file);
  if (!parsed) return 0;
  if (kill(watch->pid, 0) == 0 || errno == EPERM) return 1;
  return 0;
}

// Returns 0 when the state is durably recorded. A watcher whose state cannot be
// read back has no ownership token, so the caller must not leave the fans forced.
static int writeWatch(const Watch *watch) {
  FILE *file = fopen(watchPath(watch->kind), "w");
  if (!file) return 1;
  int written;
  if (watch->kind == WATCH_COOL)
    written = fprintf(file, "%d %.2f %.2f %.2f\n", watch->pid, watch->percent, watch->below, watch->ceiling);
  else
    written = fprintf(file, "%d %.2f %.2f %.2f %.2f %d %d\n", watch->pid, watch->on, watch->percent,
                      watch->below, watch->ceiling, watch->sensor, watch->holding);
  int failed = written < 0;
  if (fclose(file)) failed = 1;
  return failed;
}

static void removeOwnWatch(WatchKind kind) {
  Watch watch;
  if (readWatch(kind, &watch) == 1 && watch.pid == (int)getpid()) unlink(watchPath(kind));
}

static void stopWatch(WatchKind kind) {
  Watch watch;
  if (readWatch(kind, &watch) == 1) {
    kill(watch.pid, SIGTERM);
    for (int attempt = 0; attempt < 40 && kill(watch.pid, 0) == 0; attempt++) usleep(25000);
    if (kill(watch.pid, 0) == 0) kill(watch.pid, SIGKILL);
  }
  unlink(watchPath(kind));
}

static void stopWatchers(void) {
  stopWatch(WATCH_COOL);
  stopWatch(WATCH_TRIGGER);
}

// One loop for both watchers: a cooldown holds until the dies are below the
// threshold, a trigger arms and holds on the hysteresis band. Only the process
// that forced the fans owns them, so every exit path hands them back first.
static void watchLoop(int count, Watch *watch) {
  for (;;) {
    usleep(COOL_POLL_MICROSECONDS);

    Watch owner;
    if (readWatch(watch->kind, &owner) != 1 || owner.pid != (int)getpid()) {
      if (watch->kind == WATCH_COOL || watch->holding) automatic(count);
      _exit(0);
    }

    double cpu, gpu;
    int cpuCount, gpuCount;
    if (temperatures(&cpu, &cpuCount, &gpu, &gpuCount) ||
        !groupPresent(watch->sensor, cpuCount, gpuCount)) {
      automatic(count);
      removeOwnWatch(watch->kind);
      _exit(0);
    }

    double hottest = reading(watch->sensor, cpu, gpu);

    if (watch->kind == WATCH_COOL) {
      if (hottest < watch->below || hottest > watch->ceiling) {
        automatic(count);
        removeOwnWatch(watch->kind);
        _exit(0);
      }
      continue;
    }

    if (!watch->holding && hottest >= watch->on) {
      if (force(count, watch->percent)) { automatic(count); removeOwnWatch(watch->kind); _exit(1); }
      watch->holding = 1;
      if (writeWatch(watch)) { automatic(count); _exit(1); }
    } else if (watch->holding && hottest > watch->ceiling) {
      automatic(count);
      removeOwnWatch(watch->kind);
      _exit(0);
    } else if (watch->holding && hottest <= watch->below) {
      // Stay owned if the release did not take, so the next tick retries it.
      if (automatic(count) == 0) {
        watch->holding = 0;
        writeWatch(watch);
      }
    }
  }
}

// Detaches the watcher and returns only once its state is recorded, so a caller
// never reports success for a watcher that cannot exist.
static int startWatch(int count, Watch *watch) {
  int channel[2];
  if (pipe(channel)) { fprintf(stderr, "fan_control: cannot start the watcher\n"); return 1; }

  pid_t pid = fork();
  if (pid < 0) {
    close(channel[0]); close(channel[1]);
    fprintf(stderr, "fan_control: cannot start the watcher\n");
    return 1;
  }
  if (pid == 0) {
    close(channel[0]);
    setsid();
    if (fork() > 0) _exit(0);

    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); if (devnull > 2) close(devnull); }

    // The parent closes its SMC user client on exit, which tears down the shared
    // connection; the detached watcher opens its own.
    IOServiceClose(connection);
    if (openSMC()) _exit(1);

    int own = (int)getpid();
    watch->pid = own;
    ssize_t reported = write(channel[1], &own, sizeof own);
    close(channel[1]);
    if (reported != (ssize_t)sizeof own) _exit(1);

    watchLoop(count, watch);
    _exit(0);
  }

  close(channel[1]);
  int child = 0;
  ssize_t reported = read(channel[0], &child, sizeof child);
  close(channel[0]);
  if (reported != (ssize_t)sizeof child || child <= 0) {
    fprintf(stderr, "fan_control: the watcher did not start\n");
    return 1;
  }

  watch->pid = child;
  if (writeWatch(watch)) {
    kill(child, SIGKILL);
    fprintf(stderr, "fan_control: cannot record the watcher state\n");
    return 1;
  }
  return 0;
}

static int cool(int count, double percent, double below, double ceiling) {
  stopWatchers();

  int code = force(count, percent);
  if (code) { automatic(count); return code; }

  Watch watch = {0};
  watch.kind = WATCH_COOL;
  watch.percent = percent;
  watch.below = below;
  watch.ceiling = ceiling;
  if (startWatch(count, &watch)) { automatic(count); return 1; }

  printf("cooling at %.0f%% until below %.0fC (background)\n", percent, below);
  return 0;
}

static const char *sensorName(int sensor) {
  return sensor == SENSOR_CPU ? "cpu" : sensor == SENSOR_GPU ? "gpu" : "max";
}

static int parseSensor(const char *name) {
  if (!strcmp(name, "cpu")) return SENSOR_CPU;
  if (!strcmp(name, "gpu")) return SENSOR_GPU;
  if (!strcmp(name, "max")) return SENSOR_HOTTEST;
  return -1;
}

// An empty group averages to 0 °C, which would read as "cold" forever: the
// trigger has to refuse a group that has no sensors at all.
static int groupPresent(int sensor, int cpuCount, int gpuCount) {
  if (sensor == SENSOR_CPU) return cpuCount > 0;
  if (sensor == SENSOR_GPU) return gpuCount > 0;
  return cpuCount > 0 || gpuCount > 0;
}

static double reading(int sensor, double cpu, double gpu) {
  if (sensor == SENSOR_CPU) return cpu;
  if (sensor == SENSOR_GPU) return gpu;
  return cpu > gpu ? cpu : gpu;
}

static int armTrigger(int count, Watch *request) {
  // Validate the request before tearing down a watcher that is already running:
  // an unarmable trigger must not destroy the previous instruction.
  double cpu, gpu;
  int cpuCount, gpuCount;
  if (temperatures(&cpu, &cpuCount, &gpu, &gpuCount)) { fprintf(stderr, "fan_control: cannot read sensors\n"); return 1; }
  if (!groupPresent(request->sensor, cpuCount, gpuCount)) {
    fprintf(stderr, "fan_control: no %s sensors to watch\n", sensorName(request->sensor));
    return 1;
  }

  stopWatchers();

  request->holding = reading(request->sensor, cpu, gpu) >= request->on;
  if (request->holding) {
    int code = force(count, request->percent);
    if (code) { automatic(count); return code; }
  } else if (automatic(count)) {
    // A trigger only forces when it fires, so anything the previous watcher left
    // forced has to go back to the firmware now.
    fprintf(stderr, "fan_control: cannot hand the fans back to the firmware\n");
    return 1;
  }

  if (startWatch(count, request)) { automatic(count); return 1; }

  printf("trigger %s at %.0fC: hold %.0f%% until below %.0fC (%s, background)\n",
         request->holding ? "holding" : "armed", request->on, request->percent,
         request->below, sensorName(request->sensor));
  return 0;
}

static int keys(const char *prefix) {
  double total;
  if (readNumber("#KEY", &total)) return 1;
  for (uint32_t i = 0; i < (uint32_t)total; i++) {
    Message in = {0}, reply = {0};
    in.command = CMD_INDEX;
    in.index = i;
    if (call(&in, &reply)) continue;

    char name[5], text[5];
    uint32_t size, type;
    uint8_t bytes[32];
    decode(reply.key, name);
    if (prefix && strncmp(name, prefix, strlen(prefix))) continue;
    if (readKey(name, &size, &type, bytes)) continue;
    decode(type, text);
    printf("%-5s %-5s %.2f\n", name, text, value(type, size, bytes));
  }
  return 0;
}

// strtod with no trailing garbage: a malformed number driving an SMC write has
// to be an error, not a coerced zero.
static int parseNumber(const char *text, double *out) {
  char *end = NULL;
  errno = 0;
  double value = strtod(text, &end);
  if (end == text || *end || errno) return 1;
  *out = value;
  return 0;
}

int main(int argc, char **argv) {
  const char *command = argc > 1 ? argv[1] : "status";
  int json = 0, withTemperatures = 0, belowGiven = 0, onGiven = 0, sensorGiven = 0, sensor = SENSOR_HOTTEST;
  double ceiling = DEFAULT_CEILING, below = DEFAULT_BELOW, onTemperature = DEFAULT_TRIGGER_ON;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--json")) json = 1;
    if (!strcmp(argv[i], "--temps")) withTemperatures = 1;
    if (!strcmp(argv[i], "--max-temp")) {
      if (i + 1 >= argc || parseNumber(argv[++i], &ceiling))
        { fprintf(stderr, "fan_control: --max-temp needs a number\n"); return 2; }
    }
    if (!strcmp(argv[i], "--below")) {
      if (i + 1 >= argc || parseNumber(argv[++i], &below))
        { fprintf(stderr, "fan_control: --below needs a number\n"); return 2; }
      belowGiven = 1;
    }
    if (!strcmp(argv[i], "--on")) {
      if (i + 1 >= argc || parseNumber(argv[++i], &onTemperature))
        { fprintf(stderr, "fan_control: --on needs a number\n"); return 2; }
      onGiven = 1;
    }
    if (!strcmp(argv[i], "--sensor")) {
      if (i + 1 >= argc || (sensor = parseSensor(argv[++i])) < 0)
        { fprintf(stderr, "fan_control: --sensor must be max, cpu or gpu\n"); return 2; }
      sensorGiven = 1;
    }
  }

  // Only the commands that take a speed read one positionally.
  int takesPercent = !strcmp(command, "set") || !strcmp(command, "cool") || !strcmp(command, "trigger");
  double positional = 100;
  int positionalGiven = 0;
  if (takesPercent && argc > 2 && argv[2][0] != '-') {
    if (parseNumber(argv[2], &positional) || positional < 0 || positional > 100)
      { fprintf(stderr, "fan_control: percent must be 0-100\n"); return 2; }
    positionalGiven = 1;
  }
  if (!strcmp(command, "trigger") && !belowGiven) below = DEFAULT_TRIGGER_BELOW;
  if (onGiven && strcmp(command, "trigger"))
    { fprintf(stderr, "fan_control: --on only applies to trigger\n"); return 2; }
  if (sensorGiven && strcmp(command, "trigger"))
    { fprintf(stderr, "fan_control: --sensor only applies to trigger\n"); return 2; }

  if (openSMC()) return 1;

  int code;
  if (!strcmp(command, "keys")) { code = keys(argc > 2 ? argv[2] : NULL); IOServiceClose(connection); return code; }

  double count;
  if (readNumber("FNum", &count) || count <= 0) { fprintf(stderr, "fan_control: cannot read FNum\n"); IOServiceClose(connection); return 1; }
  int fans = (int)count > MAX_FANS ? MAX_FANS : (int)count;

  int writes = !strcmp(command, "max") || !strcmp(command, "auto") || !strcmp(command, "guard") ||
               !strcmp(command, "cool") || !strcmp(command, "trigger") ||
               (!strcmp(command, "set") && positionalGiven);

  if (!strcmp(command, "cool")) {
    if (below <= 0 || below >= 130) { fprintf(stderr, "fan_control: --below must be 0-130\n"); IOServiceClose(connection); return 2; }
    if (below >= ceiling) { fprintf(stderr, "fan_control: --below must be under the %.0fC ceiling\n", ceiling); IOServiceClose(connection); return 2; }
  }
  if (!strcmp(command, "trigger")) {
    if (below <= 0 || below >= 130) { fprintf(stderr, "fan_control: --below must be 0-130\n"); IOServiceClose(connection); return 2; }
    if (onTemperature <= 0 || onTemperature >= 130) { fprintf(stderr, "fan_control: --on must be 0-130\n"); IOServiceClose(connection); return 2; }
    if (onTemperature <= below) { fprintf(stderr, "fan_control: --on must be above --below\n"); IOServiceClose(connection); return 2; }
    if (onTemperature >= ceiling) { fprintf(stderr, "fan_control: --on must be under the %.0fC ceiling\n", ceiling); IOServiceClose(connection); return 2; }
  }
  if (writes && geteuid() != 0) { fprintf(stderr, "fan_control: %s needs root\n", command); IOServiceClose(connection); return 1; }

  if (!strcmp(command, "status")) code = status(fans, json, withTemperatures);
  else if (!strcmp(command, "max")) { stopWatchers(); code = force(fans, 100); }
  else if (!strcmp(command, "auto")) { stopWatchers(); code = automatic(fans); }
  else if (!strcmp(command, "set") && positionalGiven) { stopWatchers(); code = force(fans, positional); }
  else if (!strcmp(command, "guard")) code = guard(fans, ceiling);
  else if (!strcmp(command, "cool")) code = cool(fans, positional, below, ceiling);
  else if (!strcmp(command, "trigger")) {
    Watch request = {0};
    request.kind = WATCH_TRIGGER;
    request.on = onTemperature;
    request.percent = positional;
    request.below = below;
    request.ceiling = ceiling;
    request.sensor = sensor;
    code = armTrigger(fans, &request);
  }
  else { fprintf(stderr, "usage: fan_control status [--json] [--temps] | set <0-100> | max | auto | guard [--max-temp C] | cool [<0-100>] [--below C] [--max-temp C] | trigger [<0-100>] [--on C] [--below C] [--sensor max|cpu|gpu] [--max-temp C] | keys [prefix]\n"); code = 2; }

  IOServiceClose(connection);
  return code;
}
