/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <bootloader_message/bootloader_message.h>
#include <cutils/android_reboot.h>
#include <private/android_logger.h> /* private pmsg functions */
#include <selinux/android.h>
#include <selinux/label.h>
#include <selinux/selinux.h>

#include "common.h"
#include "device.h"
#include "logging.h"
#include "minadbd/minadbd.h"
#include "otautil/paths.h"
#include "otautil/sysutil.h"
#include "recovery.h"
#include "roots.h"
#include "stub_ui.h"
#include "ui.h"

static constexpr const char* COMMAND_FILE = "/cache/recovery/command";
static constexpr const char* LOCALE_FILE = "/cache/recovery/last_locale";

static constexpr const char* CACHE_ROOT = "/cache";

bool has_cache = false;

RecoveryUI* ui = nullptr;
struct selabel_handle* sehandle;

static void UiLogger(android::base::LogId /* id */, android::base::LogSeverity severity,
                     const char* /* tag */, const char* /* file */, unsigned int /* line */,
                     const char* message) {
  static constexpr char log_characters[] = "VDIWEF";
  if (severity >= android::base::ERROR && ui != nullptr) {
    ui->Print("E:%s\n", message);
  } else {
    fprintf(stdout, "%c:%s\n", log_characters[severity], message);
  }
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static std::vector<std::string> get_args(const int argc, char** const argv) {
  CHECK_GT(argc, 0);

  bootloader_message boot = {};
  std::string err;
  if (!read_bootloader_message(&boot, &err)) {
    LOG(ERROR) << err;
    // If fails, leave a zeroed bootloader_message.
    boot = {};
  }
  stage = std::string(boot.stage);

  if (boot.command[0] != 0) {
    std::string boot_command = std::string(boot.command, sizeof(boot.command));
    LOG(INFO) << "Boot command: " << boot_command;
  }

  if (boot.status[0] != 0) {
    std::string boot_status = std::string(boot.status, sizeof(boot.status));
    LOG(INFO) << "Boot status: " << boot_status;
  }

  std::vector<std::string> args(argv, argv + argc);

  // --- if arguments weren't supplied, look in the bootloader control block
  if (args.size() == 1) {
    boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
    std::string boot_recovery(boot.recovery);
    std::vector<std::string> tokens = android::base::Split(boot_recovery, "\n");
    if (!tokens.empty() && tokens[0] == "recovery") {
      for (auto it = tokens.begin() + 1; it != tokens.end(); it++) {
        // Skip empty and '\0'-filled tokens.
        if (!it->empty() && (*it)[0] != '\0') args.push_back(std::move(*it));
      }
      LOG(INFO) << "Got " << args.size() << " arguments from boot message";
    } else if (boot.recovery[0] != 0) {
      LOG(ERROR) << "Bad boot message: \"" << boot_recovery << "\"";
    }
  }

  // --- if that doesn't work, try the command file (if we have /cache).
  if (args.size() == 1 && has_cache) {
    std::string content;
    if (ensure_path_mounted(COMMAND_FILE) == 0 &&
        android::base::ReadFileToString(COMMAND_FILE, &content)) {
      std::vector<std::string> tokens = android::base::Split(content, "\n");
      // All the arguments in COMMAND_FILE are needed (unlike the BCB message,
      // COMMAND_FILE doesn't use filename as the first argument).
      for (auto it = tokens.begin(); it != tokens.end(); it++) {
        // Skip empty and '\0'-filled tokens.
        if (!it->empty() && (*it)[0] != '\0') args.push_back(std::move(*it));
      }
      LOG(INFO) << "Got " << args.size() << " arguments from " << COMMAND_FILE;
    }
  }

  // Write the arguments (excluding the filename in args[0]) back into the
  // bootloader control block. So the device will always boot into recovery to
  // finish the pending work, until finish_recovery() is called.
  std::vector<std::string> options(args.cbegin() + 1, args.cend());
  if (!update_bootloader_message(options, &err)) {
    LOG(ERROR) << "Failed to set BCB message: " << err;
  }

  return args;
}

static std::string load_locale_from_cache() {
  if (ensure_path_mounted(LOCALE_FILE) != 0) {
    LOG(ERROR) << "Can't mount " << LOCALE_FILE;
    return "";
  }

  std::string content;
  if (!android::base::ReadFileToString(LOCALE_FILE, &content)) {
    PLOG(ERROR) << "Can't read " << LOCALE_FILE;
    return "";
  }

  return android::base::Trim(content);
}

static void redirect_stdio(const char* filename) {
  int pipefd[2];
  if (pipe(pipefd) == -1) {
    PLOG(ERROR) << "pipe failed";

    // Fall back to traditional logging mode without timestamps. If these fail, there's not really
    // anywhere to complain...
    freopen(filename, "a", stdout);
    setbuf(stdout, nullptr);
    freopen(filename, "a", stderr);
    setbuf(stderr, nullptr);

    return;
  }

  pid_t pid = fork();
  if (pid == -1) {
    PLOG(ERROR) << "fork failed";

    // Fall back to traditional logging mode without timestamps. If these fail, there's not really
    // anywhere to complain...
    freopen(filename, "a", stdout);
    setbuf(stdout, nullptr);
    freopen(filename, "a", stderr);
    setbuf(stderr, nullptr);

    return;
  }

  if (pid == 0) {
    /// Close the unused write end.
    close(pipefd[1]);

    auto start = std::chrono::steady_clock::now();

    // Child logger to actually write to the log file.
    FILE* log_fp = fopen(filename, "ae");
    if (log_fp == nullptr) {
      PLOG(ERROR) << "fopen \"" << filename << "\" failed";
      close(pipefd[0]);
      _exit(EXIT_FAILURE);
    }

    FILE* pipe_fp = fdopen(pipefd[0], "r");
    if (pipe_fp == nullptr) {
      PLOG(ERROR) << "fdopen failed";
      check_and_fclose(log_fp, filename);
      close(pipefd[0]);
      _exit(EXIT_FAILURE);
    }

    char* line = nullptr;
    size_t len = 0;
    while (getline(&line, &len, pipe_fp) != -1) {
      auto now = std::chrono::steady_clock::now();
      double duration =
          std::chrono::duration_cast<std::chrono::duration<double>>(now - start).count();
      if (line[0] == '\n') {
        fprintf(log_fp, "[%12.6lf]\n", duration);
      } else {
        fprintf(log_fp, "[%12.6lf] %s", duration, line);
      }
      fflush(log_fp);
    }

    PLOG(ERROR) << "getline failed";

    free(line);
    check_and_fclose(log_fp, filename);
    close(pipefd[0]);
    _exit(EXIT_FAILURE);
  } else {
    // Redirect stdout/stderr to the logger process. Close the unused read end.
    close(pipefd[0]);

    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);

    if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
      PLOG(ERROR) << "dup2 stdout failed";
    }
    if (dup2(pipefd[1], STDERR_FILENO) == -1) {
      PLOG(ERROR) << "dup2 stderr failed";
    }

    close(pipefd[1]);
  }
}

int main(int argc, char** argv) {
  // We don't have logcat yet under recovery; so we'll print error on screen and log to stdout
  // (which is redirected to recovery.log) as we used to do.
  android::base::InitLogging(argv, &UiLogger);

  // Take last pmsg contents and rewrite it to the current pmsg session.
  static constexpr const char filter[] = "recovery/";
  // Do we need to rotate?
  bool do_rotate = false;

  __android_log_pmsg_file_read(LOG_ID_SYSTEM, ANDROID_LOG_INFO, filter, logbasename, &do_rotate);
  // Take action to refresh pmsg contents
  __android_log_pmsg_file_read(LOG_ID_SYSTEM, ANDROID_LOG_INFO, filter, logrotate, &do_rotate);

  // If this binary is started with the single argument "--adbd", instead of being the normal
  // recovery binary, it turns into kind of a stripped-down version of adbd that only supports the
  // 'sideload' command.  Note this must be a real argument, not anything in the command file or
  // bootloader control block; the only way recovery should be run with this argument is when it
  // starts a copy of itself from the apply_from_adb() function.
  if (argc == 2 && strcmp(argv[1], "--adbd") == 0) {
    minadbd_main();
    return 0;
  }

  time_t start = time(nullptr);

  // redirect_stdio should be called only in non-sideload mode. Otherwise we may have two logger
  // instances with different timestamps.
  redirect_stdio(Paths::Get().temporary_log_file().c_str());

  printf("Starting recovery (pid %d) on %s", getpid(), ctime(&start));

  load_volume_table();
  has_cache = volume_for_mount_point(CACHE_ROOT) != nullptr;

  std::vector<std::string> args = get_args(argc, argv);
  std::vector<char*> args_to_parse(args.size());
  std::transform(args.cbegin(), args.cend(), args_to_parse.begin(),
                 [](const std::string& arg) { return const_cast<char*>(arg.c_str()); });

  static constexpr struct option OPTIONS[] = {
    { "locale", required_argument, nullptr, 0 },
    { "show_text", no_argument, nullptr, 't' },
    { nullptr, 0, nullptr, 0 },
  };

  bool show_text = false;
  std::string locale;

  int arg;
  int option_index;
  while ((arg = getopt_long(args_to_parse.size(), args_to_parse.data(), "", OPTIONS,
                            &option_index)) != -1) {
    switch (arg) {
      case 't':
        show_text = true;
        break;
      case 0: {
        std::string option = OPTIONS[option_index].name;
        if (option == "locale") {
          locale = optarg;
        }
        break;
      }
    }
  }
  optind = 1;

  if (locale.empty()) {
    if (has_cache) {
      locale = load_locale_from_cache();
    }

    if (locale.empty()) {
      static constexpr const char* DEFAULT_LOCALE = "en-US";
      locale = DEFAULT_LOCALE;
    }
  }

  printf("locale is [%s]\n", locale.c_str());

  Device* device = make_device();
  if (android::base::GetBoolProperty("ro.boot.quiescent", false)) {
    printf("Quiescent recovery mode.\n");
    device->ResetUI(new StubRecoveryUI());
  } else {
    if (!device->GetUI()->Init(locale)) {
      printf("Failed to initialize UI; using stub UI instead.\n");
      device->ResetUI(new StubRecoveryUI());
    }
  }
  ui = device->GetUI();

  if (!has_cache) {
    device->RemoveMenuItemForAction(Device::WIPE_CACHE);
  }

  ui->SetBackground(RecoveryUI::NONE);
  if (show_text) ui->ShowText(true);

  sehandle = selinux_android_file_context_handle();
  selinux_android_set_sehandle(sehandle);
  if (!sehandle) {
    ui->Print("Warning: No file_contexts\n");
  }

  Device::BuiltinAction after = start_recovery(device, args);

  switch (after) {
    case Device::SHUTDOWN:
      ui->Print("Shutting down...\n");
      android::base::SetProperty(ANDROID_RB_PROPERTY, "shutdown,");
      break;

    case Device::REBOOT_BOOTLOADER:
      ui->Print("Rebooting to bootloader...\n");
      android::base::SetProperty(ANDROID_RB_PROPERTY, "reboot,bootloader");
      break;

    default:
      ui->Print("Rebooting...\n");
      reboot("reboot,");
      break;
  }
  while (true) {
    pause();
  }
  // Should be unreachable.
  return EXIT_SUCCESS;
}
