#import <Foundation/Foundation.h>

void nm_coreml_sweep_run(NSString *base, void (^emit)(NSString *line), void (^done)(NSString *all));
