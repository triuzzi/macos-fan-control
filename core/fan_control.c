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
#define COOL_POLL_MICROSECONDS 1000000
#ifndef COOL_STATE
#define COOL_STATE "/var/run/fan_control.cool"
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

static io_connect_t connection;
static int readCoolState(int *pid, double *percent, double *below, double *ceiling);
static void stopCool(void);

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

  int coolPid = 0; double coolPercent = 0, coolBelow = 0, coolCeiling = 0;
  int cooling = readCoolState(&coolPid, &coolPercent, &coolBelow, &coolCeiling);

  if (!json) {
    for (int i = 0; i < count; i++)
      printf("fan %d  %.0f rpm  %.0f%%  target %.0f  %s\n", i, fans[i].actual,
             percent(&fans[i], fans[i].actual), fans[i].target, fans[i].forced ? "forced" : "auto");
    if (withTemperatures) printf("cpu %.1fC  gpu %.1fC\n", cpu, gpu);
    if (cooling) printf("cooling %.0f%% until below %.0fC (pid %d)\n", coolPercent, coolBelow, coolPid);
    return 0;
  }

  printf("{\"fanCount\":%d,\"fans\":[", count);
  for (int i = 0; i < count; i++)
    printf("%s{\"index\":%d,\"actual\":%.0f,\"minimum\":%.0f,\"maximum\":%.0f,\"target\":%.0f,\"percent\":%.1f,\"forced\":%s}",
           i ? "," : "", i, fans[i].actual, fans[i].minimum, fans[i].maximum, fans[i].target,
           percent(&fans[i], fans[i].actual), fans[i].forced ? "true" : "false");
  printf("]");
  if (cooling)
    printf(",\"cooling\":{\"percent\":%.0f,\"below\":%.0f,\"pid\":%d}", coolPercent, coolBelow, coolPid);
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
  stopCool();
  return automatic(count);
}

static int readCoolState(int *pid, double *percent, double *below, double *ceiling) {
  FILE *file = fopen(COOL_STATE, "r");
  if (!file) return 0;
  int parsed = fscanf(file, "%d %lf %lf %lf", pid, percent, below, ceiling) == 4;
  fclose(file);
  if (!parsed) return 0;
  if (kill(*pid, 0) == 0 || errno == EPERM) return 1;
  return 0;
}

static void writeCoolState(double percent, double below, double ceiling) {
  FILE *file = fopen(COOL_STATE, "w");
  if (!file) return;
  fprintf(file, "%d %.2f %.2f %.2f\n", (int)getpid(), percent, below, ceiling);
  fclose(file);
}

static void removeOwnCoolState(void) {
  int pid; double percent, below, ceiling;
  if (readCoolState(&pid, &percent, &below, &ceiling) == 1 && pid == (int)getpid()) unlink(COOL_STATE);
}

static void stopCool(void) {
  int pid; double percent, below, ceiling;
  if (readCoolState(&pid, &percent, &below, &ceiling) == 1) {
    kill(pid, SIGTERM);
    for (int attempt = 0; attempt < 40 && kill(pid, 0) == 0; attempt++) usleep(25000);
    if (kill(pid, 0) == 0) kill(pid, SIGKILL);
  }
  unlink(COOL_STATE);
}

static int startCool(int count, double percent, double below, double ceiling) {
  pid_t pid = fork();
  if (pid < 0) { fprintf(stderr, "fan_control: cannot start cooler\n"); return 1; }
  if (pid > 0) return 0;

  setsid();
  if (fork() > 0) _exit(0);

  int devnull = open("/dev/null", O_RDWR);
  if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2); if (devnull > 2) close(devnull); }

  // The parent closes its SMC user client on exit, which tears down the shared
  // connection; the detached watcher opens its own.
  IOServiceClose(connection);
  if (openSMC()) _exit(1);

  writeCoolState(percent, below, ceiling);

  for (;;) {
    usleep(COOL_POLL_MICROSECONDS);

    int owner; double ownerPercent, ownerBelow, ownerCeiling;
    if (readCoolState(&owner, &ownerPercent, &ownerBelow, &ownerCeiling) != 1 || owner != (int)getpid()) _exit(0);

    double cpu, gpu;
    int cpuCount, gpuCount;
    if (temperatures(&cpu, &cpuCount, &gpu, &gpuCount)) { automatic(count); removeOwnCoolState(); _exit(0); }

    double hottest = cpu > gpu ? cpu : gpu;
    if (hottest < below || hottest > ceiling) { automatic(count); removeOwnCoolState(); _exit(0); }
  }
}

static int cool(int count, double percent, double below, double ceiling) {
  stopCool();

  int code = force(count, percent);
  if (code) return code;
  if (startCool(count, percent, below, ceiling)) return 1;

  printf("cooling at %.0f%% until below %.0fC (background)\n", percent, below);
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

int main(int argc, char **argv) {
  const char *command = argc > 1 ? argv[1] : "status";
  int json = 0, withTemperatures = 0;
  double ceiling = DEFAULT_CEILING, below = DEFAULT_BELOW;
  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--json")) json = 1;
    if (!strcmp(argv[i], "--temps")) withTemperatures = 1;
    if (!strcmp(argv[i], "--max-temp") && i + 1 < argc) ceiling = atof(argv[++i]);
    if (!strcmp(argv[i], "--below") && i + 1 < argc) below = atof(argv[++i]);
  }

  if (openSMC()) return 1;

  int code;
  if (!strcmp(command, "keys")) { code = keys(argc > 2 ? argv[2] : NULL); IOServiceClose(connection); return code; }

  double count;
  if (readNumber("FNum", &count) || count <= 0) { fprintf(stderr, "fan_control: cannot read FNum\n"); IOServiceClose(connection); return 1; }
  int fans = (int)count > MAX_FANS ? MAX_FANS : (int)count;

  int writes = !strcmp(command, "max") || !strcmp(command, "auto") || !strcmp(command, "guard") ||
               !strcmp(command, "cool") || (!strcmp(command, "set") && argc > 2);

  if (!strcmp(command, "set") && argc > 2 && (atof(argv[2]) < 0 || atof(argv[2]) > 100))
    { fprintf(stderr, "fan_control: percent must be 0-100\n"); IOServiceClose(connection); return 2; }
  if (!strcmp(command, "cool")) {
    if (below <= 0 || below >= 130) { fprintf(stderr, "fan_control: --below must be 0-130\n"); IOServiceClose(connection); return 2; }
    if (below >= ceiling) { fprintf(stderr, "fan_control: --below must be under the %.0fC ceiling\n", ceiling); IOServiceClose(connection); return 2; }
  }
  if (writes && geteuid() != 0) { fprintf(stderr, "fan_control: %s needs root\n", command); IOServiceClose(connection); return 1; }

  if (!strcmp(command, "status")) code = status(fans, json, withTemperatures);
  else if (!strcmp(command, "max")) { stopCool(); code = force(fans, 100); }
  else if (!strcmp(command, "auto")) { stopCool(); code = automatic(fans); }
  else if (!strcmp(command, "set") && argc > 2) { stopCool(); code = force(fans, atof(argv[2])); }
  else if (!strcmp(command, "guard")) code = guard(fans, ceiling);
  else if (!strcmp(command, "cool")) code = cool(fans, argc > 2 && argv[2][0] != '-' ? atof(argv[2]) : 100, below, ceiling);
  else { fprintf(stderr, "usage: fan_control status [--json] [--temps] | set <0-100> | max | auto | guard [--max-temp C] | cool [<0-100>] [--below C] [--max-temp C] | keys [prefix]\n"); code = 2; }

  IOServiceClose(connection);
  return code;
}
