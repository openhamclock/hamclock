//
//  HamClockBridge.h
//  HamClock
//
//  Objective-C++ Bridge to native C++ HamClock Core Engine.
//

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@protocol HamClockBridgeDelegate <NSObject>
- (void)hamclockExitRequested NS_SWIFT_NAME(hamclockExitRequested());
- (void)hamclockRestartRequestedWithMinusK:(BOOL)minusK NS_SWIFT_NAME(hamclockRestartRequested(minusK:));
- (void)hamclockOpenURL:(NSURL *)url NS_SWIFT_NAME(hamclockOpenURL(_:));
- (NSString *)hamclockGetClipboardText NS_SWIFT_NAME(hamclockGetClipboardText());
@end

@interface HamClockBridge : NSObject

@property (nonatomic, weak, nullable) id<HamClockBridgeDelegate> delegate;

+ (instancetype)shared;

- (BOOL)startDaemonWithDataDir:(NSString *)dataDir
                        rwPort:(int)rwPort
                        roPort:(int)roPort
                      restPort:(int)restPort
                   backendHost:(nullable NSString *)backendHost
                   hasLocation:(BOOL)hasLocation
                           lat:(double)lat
                           lng:(double)lng
                    forceSetup:(BOOL)forceSetup
                countdownSetup:(BOOL)countdownSetup;

- (BOOL)isDaemonRunning;

- (void)setAllowExternalAccess:(BOOL)allow;

- (nullable UIImage *)generateQRCodeImageForText:(NSString *)text
                                           scale:(int)scale
                                          border:(int)border;

@end

NS_ASSUME_NONNULL_END
