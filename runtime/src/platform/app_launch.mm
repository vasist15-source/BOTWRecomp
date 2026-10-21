#import <Foundation/Foundation.h>
#include <TargetConditionals.h>
#include "app_launch.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#if TARGET_OS_OSX
[[noreturn]] static void fail(NSString* message) {
    CFUserNotificationDisplayAlert(0, kCFUserNotificationStopAlertLevel, nullptr, nullptr, nullptr,
                                   CFSTR("Breath of the Wild can't start"), (__bridge CFStringRef)message,
                                   nullptr, nullptr, nullptr, nullptr);
    exit(1);
}

static NSString* str(NSDictionary* d, NSString* key) {
    id v = d[key];
    return [v isKindOfClass:[NSString class]] && [v length] ? v : nil;
}

static bool is_dir(NSString* path) {
    BOOL dir = NO;
    return path && [[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&dir] && dir;
}

void app_bundle_launch(int& argc, char**& argv) {
    @autoreleasepool {
        NSBundle* bundle = [NSBundle mainBundle];
        if (![bundle.bundlePath hasSuffix:@".app"]) return;
        for (int i = 1; i < argc; i++)
            if (!strcmp(argv[i], "--game")) return;
        NSFileManager* fm = [NSFileManager defaultManager];
        NSString* support = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/BotW"];
        NSString* logs = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Logs/BotW"];
        NSString *game, *base, *aoc, *save, *dir;
        if (NSDictionary* l = [bundle objectForInfoDictionaryKey:@"BotWLaunch"]; [l isKindOfClass:[NSDictionary class]]) {
            game = str(l, @"game"), base = str(l, @"base"), aoc = str(l, @"aoc"), save = str(l, @"save"), dir = str(l, @"dir");
        } else {
            NSDictionary* cfg = [NSDictionary dictionaryWithContentsOfFile:[support stringByAppendingPathComponent:@"install.plist"]];
            if (!cfg) fail(@"The game files are not set up. Open the Breath of the Wild installer and build the game again.");
            game = str(cfg, @"update"), base = str(cfg, @"base"), aoc = str(cfg, @"dlc"), save = str(cfg, @"save"), dir = nil;
        }
        if (!is_dir([base stringByAppendingPathComponent:@"content"]))
            fail([NSString stringWithFormat:@"The base game is not at %@ anymore. Connect the drive it is on, or run the installer again and choose its new place.", base ?: @"(not set)"]);
        if (!is_dir([game stringByAppendingPathComponent:@"code"]))
            fail([NSString stringWithFormat:@"The update is not at %@ anymore. Connect the drive it is on, or run the installer again and choose its new place.", game ?: @"(not set)"]);
        if (!save) save = [support stringByAppendingPathComponent:@"save"];
        [fm createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:nil];
        [fm createDirectoryAtPath:save withIntermediateDirectories:YES attributes:nil error:nil];
        if (chdir((dir ?: logs).fileSystemRepresentation) != 0) exit(1);

        NSString* log = [logs stringByAppendingPathComponent:@"wwhd.log"];
        rename(log.fileSystemRepresentation, [logs stringByAppendingPathComponent:@"wwhd.previous.log"].fileSystemRepresentation);
        if (freopen(log.fileSystemRepresentation, "w", stdout)) dup2(fileno(stdout), fileno(stderr));
        setvbuf(stdout, nullptr, _IOLBF, 0);

        // argv[0], the game's folders, then whatever the app was opened with (never the old -psn_ argument)
        static std::vector<std::string> keep;
        static std::vector<char*> args;
        keep = {argv[0], "--game", game.fileSystemRepresentation, "--base", base.fileSystemRepresentation,
                "--save", save.fileSystemRepresentation};
        if (is_dir([aoc stringByAppendingPathComponent:@"content"])) {
            keep.push_back("--aoc");
            keep.push_back(aoc.fileSystemRepresentation);
        }
        for (int i = 1; i < argc; i++)
            if (strncmp(argv[i], "-psn_", 5)) keep.push_back(argv[i]);
        for (auto& s : keep) args.push_back(s.data());
        args.push_back(nullptr);
        argc = (int)keep.size();
        argv = args.data();
    }
}
#else
void app_bundle_launch(int&, char**&) {}
#endif
