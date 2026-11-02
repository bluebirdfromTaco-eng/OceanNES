/* Game Genie codes: per-game list and the window that edits it */
#import <Cocoa/Cocoa.h>
#include "../src/nes.h"

@interface CheatManager : NSObject <NSTableViewDataSource, NSTableViewDelegate>
+ (instancetype)shared;
/* called with the enabled, decoded codes whenever they change */
@property (nonatomic, copy) void (^apply)(const Cheat *codes, int count);
/* key identifies the game (nil = no game loaded) */
- (void)loadForGame:(NSString *)key name:(NSString *)name;
- (int)activeCount;
- (void)showWindow;
@end
