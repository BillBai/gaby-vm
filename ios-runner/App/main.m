// Copyright 2026, the gaby-vm authors
// SPDX-License-Identifier: BSD-3-Clause
//
// Minimal host application for the gaby-vm iOS runner. It does nothing but
// launch; its only job is to host the XCTest bundle (and the on-device
// benchmark run). Kept deliberately tiny.
//
// It adopts the UIScene lifecycle — a scene delegate plus a generated scene
// manifest (INFOPLIST_KEY_UIApplicationSceneManifest_Generation in
// project.yml) — rather than the legacy app-delegate window lifecycle, which
// UIKit now warns is on its way to becoming a hard launch assertion.

#import <UIKit/UIKit.h>

// Owns the host window for a connected scene. A blank root view controller is
// all the test host needs on screen.
@interface SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(strong, nonatomic) UIWindow *window;
@end

@implementation SceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)connectionOptions {
  UIWindowScene *windowScene = (UIWindowScene *)scene;
  self.window = [[UIWindow alloc] initWithWindowScene:windowScene];
  UIViewController *root = [[UIViewController alloc] init];
  root.view.backgroundColor = UIColor.systemBackgroundColor;
  self.window.rootViewController = root;
  [self.window makeKeyAndVisible];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
  return YES;
}

// The generated scene manifest declares scene support but no configuration, so
// UIKit asks here which delegate drives a connecting scene.
- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession
                                   options:(UISceneConnectionOptions *)options {
  UISceneConfiguration *config =
      [UISceneConfiguration configurationWithName:@"Default Configuration"
                                      sessionRole:connectingSceneSession.role];
  config.delegateClass = [SceneDelegate class];
  return config;
}
@end

int main(int argc, char *argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass([AppDelegate class]));
  }
}
