// iOS side of the phone layer (native.h). Compiled with ARC.
#include "mobile/native.h"

#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <mach/mach.h>
#include <os/log.h>
#include <os/proc.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

#include <atomic>
#include <mutex>

namespace fs = std::filesystem;

// ---- Haptics ---------------------------------------------------------------------------------

@interface LMHaptics : NSObject
@property(nonatomic, strong) CHHapticEngine* engine;
@property(nonatomic, strong) id<CHHapticAdvancedPatternPlayer> player;
@property(nonatomic) BOOL running;
@end

@implementation LMHaptics
- (BOOL)ensure {
  if (!CHHapticEngine.capabilitiesForHardware.supportsHaptics) return NO;
  NSError* err = nil;
  if (self.engine == nil) {
    self.engine = [[CHHapticEngine alloc] initAndReturnError:&err];
    if (err != nil || self.engine == nil) {
      self.engine = nil;
      return NO;
    }
    self.engine.playsHapticsOnly = YES;
    __weak LMHaptics* weakSelf = self;
    self.engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
      weakSelf.player = nil;
      weakSelf.running = NO;
    };
    self.engine.resetHandler = ^{
      weakSelf.player = nil;
      weakSelf.running = NO;
      [weakSelf.engine startAndReturnError:nil];
    };
    [self.engine startAndReturnError:&err];
    if (err != nil) {
      self.engine = nil;
      return NO;
    }
  }
  if (self.player == nil) {
    // A long continuous event at a moderate strength: the GameCube motor is on/off only.
    CHHapticEventParameter* intensity =
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:0.75f];
    CHHapticEventParameter* sharpness =
        [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:0.25f];
    CHHapticEvent* ev = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                       parameters:@[ intensity, sharpness ]
                                                     relativeTime:0
                                                         duration:30.0];
    CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[ ev ] parameters:@[] error:&err];
    if (pattern == nil) return NO;
    self.player = [self.engine createAdvancedPlayerWithPattern:pattern error:&err];
    if (self.player == nil) return NO;
    self.player.loopEnabled = YES;
  }
  return YES;
}
- (void)setOn:(BOOL)on {
  if (on == self.running) return;
  if (on) {
    if (![self ensure]) return;
    if ([self.player startAtTime:CHHapticTimeImmediate error:nil]) self.running = YES;
  } else {
    [self.player stopAtTime:CHHapticTimeImmediate error:nil];
    self.running = NO;
  }
}
@end

// ---- Document picker -------------------------------------------------------------------------

@interface LMDiscPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic, copy) void (^done)(NSURL* _Nullable url);
@end

@implementation LMDiscPicker
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  if (self.done) self.done(urls.firstObject);
  self.done = nil;
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  if (self.done) self.done(nil);
  self.done = nil;
}
@end

namespace {

LMHaptics* sHaptics = nil;
LMDiscPicker* sPicker = nil;
std::atomic<bool> sRumbleWanted{false};

UIViewController* topController() {
  UIWindow* key = nil;
  for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
    if (![scene isKindOfClass:[UIWindowScene class]]) continue;
    for (UIWindow* w in ((UIWindowScene*)scene).windows)
      if (w.isKeyWindow) key = w;
  }
  if (key == nil) {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes)
      if ([scene isKindOfClass:[UIWindowScene class]]) key = ((UIWindowScene*)scene).windows.firstObject;
  }
  UIViewController* vc = key.rootViewController;
  while (vc.presentedViewController != nil) vc = vc.presentedViewController;
  return vc;
}

void onMain(void (^block)(void)) {
  if ([NSThread isMainThread]) block();
  else dispatch_async(dispatch_get_main_queue(), block);
}

fs::path pathOf(NSURL* url) { return fs::path(std::string(url.fileSystemRepresentation)); }

}  // namespace

namespace mobile::native {

bool isIOS() { return true; }

void haptic(int style) {
  onMain(^{
    const UIImpactFeedbackStyle st = style >= 2   ? UIImpactFeedbackStyleHeavy
                                     : style == 1 ? UIImpactFeedbackStyleMedium
                                                  : UIImpactFeedbackStyleLight;
    UIImpactFeedbackGenerator* g = [[UIImpactFeedbackGenerator alloc] initWithStyle:st];
    [g impactOccurred];
  });
}

void rumble(bool on) {
  if (sRumbleWanted.exchange(on) == on) return;
  onMain(^{
    if (sHaptics == nil) sHaptics = [[LMHaptics alloc] init];
    [sHaptics setOn:sRumbleWanted.load() ? YES : NO];
  });
}

int thermalState() { return int(NSProcessInfo.processInfo.thermalState); }

long long footprintMB() {
  task_vm_info_data_t info;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) return -1;
  return static_cast<long long>(info.phys_footprint / (1024 * 1024));
}

long long availableMemoryMB() { return static_cast<long long>(os_proc_available_memory() / (1024 * 1024)); }

long long physicalMemoryMB() { return static_cast<long long>(NSProcessInfo.processInfo.physicalMemory / (1024 * 1024)); }

long long freeDiskMB() {
  NSURL* home = [NSURL fileURLWithPath:NSHomeDirectory()];
  NSDictionary* v = [home resourceValuesForKeys:@[ NSURLVolumeAvailableCapacityForImportantUsageKey ] error:nil];
  NSNumber* n = v[NSURLVolumeAvailableCapacityForImportantUsageKey];
  return n != nil ? n.longLongValue / (1024 * 1024) : -1;
}

bool lowPowerMode() { return NSProcessInfo.processInfo.lowPowerModeEnabled; }

std::string deviceModel() {
  utsname u{};
  uname(&u);
  return u.machine;
}

std::string osVersion() {
  return std::string(UIDevice.currentDevice.systemName.UTF8String) + " " + UIDevice.currentDevice.systemVersion.UTF8String;
}

std::string appVersion() {
  NSDictionary* info = NSBundle.mainBundle.infoDictionary;
  NSString* v = info[@"CFBundleShortVersionString"] ?: @"?";
  NSString* b = info[@"CFBundleVersion"] ?: @"?";
  return std::string(v.UTF8String) + " (" + b.UTF8String + ")";
}

std::string gpuName() {
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  if (dev == nil) return "no Metal device";
  std::string s = dev.name.UTF8String;
  if ([dev supportsFamily:MTLGPUFamilyApple9]) s += " (Apple9)";
  else if ([dev supportsFamily:MTLGPUFamilyApple8]) s += " (Apple8)";
  else if ([dev supportsFamily:MTLGPUFamilyApple7]) s += " (Apple7)";
  else if ([dev supportsFamily:MTLGPUFamilyApple6]) s += " (Apple6)";
  s += ", recommended working set " + std::to_string(dev.recommendedMaxWorkingSetSize / (1024 * 1024)) + " MB";
  return s;
}

int maxRefreshRate() { return int(UIScreen.mainScreen.maximumFramesPerSecond); }

fs::path documentsDir() {
  NSURL* u = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
  return u ? pathOf(u) : fs::path(NSHomeDirectory().UTF8String) / "Documents";
}

fs::path cachesDir() {
  NSURL* u = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
  return u ? pathOf(u) : fs::path(NSHomeDirectory().UTF8String) / "Library" / "Caches";
}

fs::path bundleDir() { return fs::path(NSBundle.mainBundle.bundlePath.UTF8String); }

void mirrorToSystemLog(const char* line, size_t len) {
  static os_log_t sLog = os_log_create("com.lmrecomp.ios", "game");
  os_log(sLog, "%{public}.*s", int(len), line);
}

void installObservers(std::function<void(const char* what)> onEvent) {
  static std::function<void(const char*)> sOnEvent;
  sOnEvent = std::move(onEvent);
  onMain(^{
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) { sOnEvent("memory warning"); }];
    [nc addObserverForName:NSProcessInfoThermalStateDidChangeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) { sOnEvent("thermal state changed"); }];
    [nc addObserverForName:NSProcessInfoPowerStateDidChangeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) { sOnEvent("low power mode changed"); }];
    [nc addObserverForName:UIApplicationWillTerminateNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) { sOnEvent("will terminate"); }];
    [nc addObserverForName:UIApplicationProtectedDataWillBecomeUnavailable
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification*) { sOnEvent("device locking"); }];
  });
}

bool pickDiscImage(std::function<void(const fs::path& picked, const std::string& error)> done) {
  __block std::function<void(const fs::path&, const std::string&)> cb = std::move(done);
  onMain(^{
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    // Disc images have no system types: accept any file (and folders are not offered), then let
    // nod decide (ISO, GCM, RVZ, WIA, CISO, GCZ, NFS, TGC).
    [types addObject:UTTypeData];
    UIDocumentPickerViewController* picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:types
                                                                                                          asCopy:YES];
    picker.allowsMultipleSelection = NO;
    picker.modalPresentationStyle = UIModalPresentationFormSheet;
    if (sPicker == nil) sPicker = [[LMDiscPicker alloc] init];
    sPicker.done = ^(NSURL* url) {
      if (url == nil) {
        cb({}, "");
        return;
      }
      // asCopy: the file is already a private copy in the app's tmp/Inbox; move it next to the saves.
      NSURL* docs = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
      NSURL* dest = [docs URLByAppendingPathComponent:url.lastPathComponent];
      NSError* err = nil;
      if ([NSFileManager.defaultManager fileExistsAtPath:dest.path])
        [NSFileManager.defaultManager removeItemAtURL:dest error:nil];
      if (![NSFileManager.defaultManager moveItemAtURL:url toURL:dest error:&err]) {
        cb(pathOf(url), std::string("could not move it into the app's folder: ") +
                            (err ? err.localizedDescription.UTF8String : "unknown error"));
        return;
      }
      cb(pathOf(dest), "");
    };
    picker.delegate = sPicker;
    UIViewController* top = topController();
    if (top == nil) {
      cb({}, "no window to show the file picker on");
      return;
    }
    [top presentViewController:picker animated:YES completion:nil];
  });
  return true;
}

}  // namespace mobile::native
