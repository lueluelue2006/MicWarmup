#import <Foundation/Foundation.h>
#import "auto_warmup.h"

typedef void (^MWMonitorUpdate)(uint64_t epoch, uint64_t binding, BOOL supported, MWAutoReading reading);

// Observes metadata on a serial worker; delivers on main, including menu/modal modes.
@interface MWDoubaoMonitor : NSObject
- (instancetype)initWithUpdate:(MWMonitorUpdate)update;
- (void)setWatching:(BOOL)watching epoch:(uint64_t)epoch;
- (void)retryBefore:(uint64_t)verifyBy epoch:(uint64_t)epoch binding:(uint64_t)binding;
@end

// A one-shot read-only capability check. It cannot start the warmup helper.
int MWCheckDoubaoMonitor(void);
