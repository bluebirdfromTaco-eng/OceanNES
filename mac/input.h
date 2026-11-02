/*
 * Input: keyboard + game controllers (GameController framework for the pads it
 * supports, raw IOKit HID for every other USB / Bluetooth gamepad), with
 * per-player keyboard bindings, per-controller-model button bindings, turbo,
 * and the Configure Controls window.
 */
#import <Cocoa/Cocoa.h>

#define INPUT_ROWS 10
/* extra bits on top of the 8 NES button bits */
#define TURBO_A_BIT 0x100u
#define TURBO_B_BIT 0x200u

@interface InputPad : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *profileKey;   /* bindings are shared per controller model */
@property (nonatomic, readonly) BOOL isHID;
- (BOOL)active:(int)vid;                            /* is a virtual input currently pressed? */
- (NSString *)nameOfInput:(int)vid;
@end

@interface InputManager : NSObject
+ (instancetype)shared;
- (void)start;
@property (nonatomic, copy) void (^notify)(NSString *msg);
@property (nonatomic, readonly) NSArray<InputPad *> *pads;   /* connected, in connection order */
- (InputPad *)padForPlayer:(int)p;
- (uint32_t)padBitsForPlayer:(int)p;
- (uint32_t)keyBitsForPlayer:(int)p;
/* keyboard events from the main window */
- (void)keyDown:(NSEvent *)e;
- (void)keyUp:(NSEvent *)e;
- (void)flagsChanged:(NSEvent *)e;
- (void)clearKeys;
/* the Configure Controls window */
- (void)showConfigWindow;
@end
