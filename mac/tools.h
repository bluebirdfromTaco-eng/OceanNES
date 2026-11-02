/* Tools menu: live RAM editor (with byte freezing) and the real-time corruptor */
#import <Cocoa/Cocoa.h>
#include <pthread.h>
#include "../src/nes.h"

extern Nes g_nes;
extern pthread_mutex_t g_lock;

/* called on the emulation thread with g_lock held, around each frame */
void tools_pre_frame(Nes *n);
void tools_post_frame(Nes *n);
/* true while the game may have been corrupted: battery saves aren't written */
bool tools_sram_blocked(void);

@interface ToolsManager : NSObject
+ (instancetype)shared;
@property (nonatomic, copy) void (^notify)(NSString *msg);
@property (nonatomic, copy) void (^powerCycle)(void);
- (void)romChanged;          /* after a ROM is loaded or closed (main thread) */
- (void)showRamEditor;
- (void)showCorruptor;
@end
