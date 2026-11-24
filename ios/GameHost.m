#import <UIKit/UIKit.h>
#import <GLKit/GLKit.h>
#import <OpenGLES/ES3/gl.h>
#include "touch_host.h"

void KitRcoreSetScreenSize(int width, int height);
void KitRcoreSetFbo(unsigned int fbo, unsigned int color);
void KitRcoreSetEaglContext(void *context);

typedef struct {
    int step, kind, souls, health, near, tool, damage, level, paused, chain;
} Battle;

static NSArray<NSNumber *> *lessons(void) { return @[@11,@12,@0,@1,@13,@14,@15]; }
static NSString *levelName(int level) {
    NSArray *names = @[@"First Drop", @"In the Shadows", @"Barrel Ambush", @"Chain Reaction",
                       @"Hold the Door", @"Crossfire", @"The Gauntlet"];
    NSUInteger i = [lessons() indexOfObject:@(level)];
    return i == NSNotFound ? [NSString stringWithFormat:@"E1M%d",level-1] : names[i];
}
static NSString *briefing(Battle b) {
    if (b.paused) return @"Paused";
    switch (b.step) {
        case 0:
            if (b.level == 11) return @"Tap the green ring to summon your first demon.";
            if (b.level == 12) return @"Summon behind the door. Hidden drops arrive instantly.";
            if (b.level == 13) return @"Set a shotgunner in the ring beyond the door.";
            if (b.level == 14) return @"Place a shotgunner at the first ring.";
            if (b.level == 15) return @"Set your ambush before he reaches the medkit.";
            return @"Drop a demon in the green ring behind the door.";
        case 1: return b.near ? @"In range! Blow the barrel." : @"Wait for the marine to approach the barrel.";
        case 2: return @"Watch your ambush play out.";
        case 3: return b.level == 15 ? @"Good job! Training complete." : @"Good job! Lesson cleared.";
        case 4: case 8: return @"The marine escaped. Try another ambush.";
        case 5: return b.tool == 1 ? @"Tap a door or lift." : b.tool == 2 ? @"Tap near a barrel." : @"Tap the map to summon. Drag to pan, pinch to zoom.";
        case 6: return @"Marine down.";
        case 7: return b.level == 9 ? @"Episode cleared!" : @"Marine down. Continue the invasion.";
        case 9: return @"The marine lost his route. Retry this map.";
        case 10: return @"Watch your demon intercept the marine.";
        case 12: return @"Slam the door to hold the marine.";
        case 13: return @"Place the second demon at the next ring.";
        default: return @"";
    }
}

static UIColor *ember(void) { return [UIColor colorWithRed:1 green:.62 blue:.34 alpha:1]; }
static UILabel *label(CGFloat points, UIFontWeight weight, UIFontTextStyle style) {
    UILabel *v = [UILabel new];
    v.numberOfLines = 0;
    v.textColor = UIColor.whiteColor;
    v.font = [[UIFontMetrics metricsForTextStyle:style] scaledFontForFont:[UIFont systemFontOfSize:points weight:weight]];
    v.adjustsFontForContentSizeCategory = YES;
    return v;
}
static UIButton *button(NSString *name, id target, SEL action) {
    UIButton *b = [UIButton buttonWithType:UIButtonTypeSystem];
    [b setTitle:name forState:UIControlStateNormal];
    b.tintColor = ember();
    b.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    b.titleLabel.adjustsFontForContentSizeCategory = YES;
    [b.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [b.widthAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    if (action) [b addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
    return b;
}
static UIAction *item(NSString *name, void (^handler)(void)) {
    return [UIAction actionWithTitle:name image:nil identifier:nil handler:^(__unused UIAction *a){handler();}];
}

@interface GameController : UIViewController <GLKViewDelegate>
@property GLKView *field;
@property CADisplayLink *clock;
@property UIScrollView *panel;
@property UIStackView *content;
@property UILabel *tip, *stats;
@property UIButton *levels, *pause, *options, *primary, *demon;
@property NSMutableDictionary<NSNumber *,NSString *> *demons;
@property NSArray *layout;
@property Battle battle;
@property BOOL initialized, failed, horizontal, muted;
@property int pixelW, pixelH, nextLevel, barrelWait, doorWait, clearTicks;
@property NSString *notice;
- (void)refresh;
- (void)episodeNext:(int)next ticks:(int)ticks barrel:(int)barrel door:(int)door;
@end

// The engine has one process-wide graphics context across scene reconnections.
static GameController *active;
@implementation GameController
- (void)viewDidLoad {
    [super viewDidLoad];
    active = self;
    self.demons = [NSMutableDictionary dictionary];
    self.nextLevel = -1; self.clearTicks = -1;
    self.view.backgroundColor = [UIColor colorWithWhite:.055 alpha:1];
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    self.muted = [NSUserDefaults.standardUserDefaults boolForKey:@"muted"];
    EAGLContext *gl = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES3];
    if (!gl) {
        UILabel *error = label(18, UIFontWeightSemibold, UIFontTextStyleBody);
        error.frame = self.view.bounds; error.textAlignment = NSTextAlignmentCenter;
        error.text = @"The 3D renderer could not start."; [self.view addSubview:error]; return;
    }
    self.field = [[GLKView alloc] initWithFrame:CGRectZero context:gl];
    self.field.drawableDepthFormat = GLKViewDrawableDepthFormat24;
    self.field.delegate = self;
    self.field.enableSetNeedsDisplay = NO;
    self.field.contentScaleFactor = 1;
    self.field.isAccessibilityElement = YES;
    self.field.accessibilityLabel = @"Battlefield";
    self.field.accessibilityTraits = UIAccessibilityTraitAllowsDirectInteraction;
    self.field.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.field];
    UITapGestureRecognizer *tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tap:)];
    UIPanGestureRecognizer *pan = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(pan:)];
    pan.maximumNumberOfTouches = 1;
    [tap requireGestureRecognizerToFail:pan];
    [self.field addGestureRecognizer:tap]; [self.field addGestureRecognizer:pan];
    [self.field addGestureRecognizer:[[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(pinch:)]];

    self.levels = button(@"First Drop", nil, NULL);
    self.levels.showsMenuAsPrimaryAction = YES;
    self.levels.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    self.levels.accessibilityHint = @"Choose a lesson or episode map";
    [self.levels setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    self.pause = button(@"Pause", self, @selector(togglePause));
    self.options = button(@"•••", nil, NULL);
    self.options.accessibilityLabel = @"Game options"; self.options.showsMenuAsPrimaryAction = YES;
    UIStackView *nav = [[UIStackView alloc] initWithArrangedSubviews:@[self.levels,self.pause,self.options]];
    nav.spacing = 8;
    self.tip = label(18, UIFontWeightSemibold, UIFontTextStyleBody);
    self.tip.text = @"Preparing the battlefield…";
    self.stats = label(14, UIFontWeightRegular, UIFontTextStyleSubheadline);
    self.stats.textColor = [UIColor colorWithWhite:.78 alpha:1];
    self.primary = button(@"",self,@selector(primaryAction));
    self.primary.backgroundColor = ember(); self.primary.layer.cornerRadius = 8;
    [self.primary setTitleColor:UIColor.blackColor forState:UIControlStateNormal];
    self.demon = button(@"Summon", nil, NULL); self.demon.showsMenuAsPrimaryAction = YES;
    self.primary.hidden = self.demon.hidden = YES;
    self.content = [[UIStackView alloc] initWithArrangedSubviews:@[nav,self.tip,self.stats,self.primary,self.demon]];
    self.content.axis = UILayoutConstraintAxisVertical; self.content.spacing = 3;
    self.content.translatesAutoresizingMaskIntoConstraints = NO;
    self.panel = [UIScrollView new]; self.panel.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.panel]; [self.panel addSubview:self.content];
    UILayoutGuide *cg = self.panel.contentLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [self.content.topAnchor constraintEqualToAnchor:cg.topAnchor constant:4],
        [self.content.bottomAnchor constraintEqualToAnchor:cg.bottomAnchor constant:-5],
        [self.content.leadingAnchor constraintEqualToAnchor:cg.leadingAnchor constant:12],
        [self.content.trailingAnchor constraintEqualToAnchor:cg.trailingAnchor constant:-12],
        [self.content.widthAnchor constraintEqualToAnchor:self.panel.frameLayoutGuide.widthAnchor constant:-24]
    ]];
    [self setPanelLayout:self.view.bounds.size];
    [self updateMenus];
    self.clock = [CADisplayLink displayLinkWithTarget:self.field selector:@selector(display)];
    self.clock.preferredFramesPerSecond = 60;
    [self.clock addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}
- (void)setPanelLayout:(CGSize)size {
    BOOL wide = size.width > size.height;
    if (self.layout && self.horizontal == wide) return;
    [NSLayoutConstraint deactivateConstraints:self.layout ?: @[]]; self.horizontal = wide;
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    if (wide) {
        self.layout = @[[self.field.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
            [self.field.topAnchor constraintEqualToAnchor:safe.topAnchor], [self.field.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor],
            [self.field.trailingAnchor constraintEqualToAnchor:self.panel.leadingAnchor],
            [self.panel.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor], [self.panel.topAnchor constraintEqualToAnchor:safe.topAnchor],
            [self.panel.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor], [self.panel.widthAnchor constraintEqualToAnchor:safe.widthAnchor multiplier:.37]];
    } else {
        NSLayoutConstraint *fit = [self.panel.heightAnchor constraintEqualToAnchor:self.content.heightAnchor constant:9];
        fit.priority = UILayoutPriorityDefaultHigh;
        self.layout = @[[self.field.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor], [self.field.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
            [self.field.topAnchor constraintEqualToAnchor:safe.topAnchor], [self.field.bottomAnchor constraintEqualToAnchor:self.panel.topAnchor],
            [self.panel.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor], [self.panel.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
            [self.panel.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor], [self.panel.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor multiplier:.5],fit];
    }
    [NSLayoutConstraint activateConstraints:self.layout];
}
- (void)viewWillLayoutSubviews { [super viewWillLayoutSubviews]; if (self.panel) [self setPanelLayout:self.view.bounds.size]; }
- (void)glkView:(GLKView *)view drawInRect:(CGRect)rect {
    if (self.failed || view.drawableWidth <= 0 || view.drawableHeight <= 0) return;
    GLint fbo; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
    KitRcoreSetFbo((unsigned)fbo,0);
    if (self.pixelW != view.drawableWidth || self.pixelH != view.drawableHeight) {
        self.pixelW = (int)view.drawableWidth; self.pixelH = (int)view.drawableHeight;
        KitRcoreSetScreenSize(self.pixelW,self.pixelH);
    }
    if (!self.initialized) {
        KitRcoreSetEaglContext((__bridge void *)view.context);
        [[NSFileManager defaultManager] changeCurrentDirectoryPath:NSBundle.mainBundle.resourcePath];
        char *args[] = {"Helldrop", NULL};
        if (HelldropIOSStart(1,args) != 0) { [self stopWithError]; return; }
        self.initialized = YES;
        WebAction(WEB_START,0); WebAction(WEB_MUTE,self.muted);
        NSNumber *last = [NSUserDefaults.standardUserDefaults objectForKey:@"lastLevel"];
        if (last && last.intValue >= 0 && last.intValue < 16 && last.intValue != 11) WebAction(WEB_CONTINUE,last.intValue);
    }
    if (!HelldropIOSFrame()) [self stopWithError];
}
- (void)stopWithError {
    self.failed = YES; self.clock.paused = YES; self.tip.text = @"The battlefield could not load.";
}
- (void)tap:(UITapGestureRecognizer *)g {
    if (!self.initialized || self.battle.paused) return;
    CGPoint p = [g locationInView:self.field]; CGSize s = self.field.bounds.size;
    if (s.width && s.height) WebTap(p.x/s.width,p.y/s.height);
}
- (void)pan:(UIPanGestureRecognizer *)g {
    if (self.battle.level < 2 || self.battle.level > 10 || self.battle.paused) return;
    CGPoint p = [g translationInView:self.field]; CGFloat h = MAX(1,self.field.bounds.size.height);
    WebPan(p.x/h,p.y/h); [g setTranslation:CGPointZero inView:self.field];
}
- (void)pinch:(UIPinchGestureRecognizer *)g {
    if (self.battle.level < 2 || self.battle.level > 10 || self.battle.paused) return;
    if (g.scale > 1.12 || g.scale < .89) { WebAction(WEB_ZOOM,g.scale > 1 ? 1 : -1); g.scale = 1; }
}
- (void)togglePause { if (self.initialized) WebAction(WEB_PAUSE,0); }
- (void)primaryAction {
    Battle b = self.battle;
    if (b.paused) return;
    if (b.step == 1) WebAction(WEB_BLAST,0);
    else if (b.step == 12) WebAction(WEB_TOOL,1);
    else if (b.step == 3) {
        NSUInteger i = [lessons() indexOfObject:@(b.level)] + 1;
        WebAction(WEB_CONTINUE,i < lessons().count ? lessons()[i].intValue : 2);
    } else if (b.step == 7 && self.nextLevel >= 0) WebAction(WEB_CONTINUE,self.nextLevel);
    else WebAction(WEB_RETRY,0);
}
- (void)updateMenus {
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    NSArray *completed = [defaults arrayForKey:@"cleared"] ?: @[];
    NSMutableArray *training = [NSMutableArray array], *episode = [NSMutableArray array];
    for (NSNumber *n in lessons()) {
        NSString *name = levelName(n.intValue);
        if ([completed containsObject:n]) name = [name stringByAppendingString:@" ✓"];
        [training addObject:item(name,^{ WebAction(WEB_CONTINUE,n.intValue); })];
    }
    for (int i=2; i<=10; i++) [episode addObject:item(levelName(i),^{WebAction(WEB_CONTINUE,i);})];
    self.levels.menu = [UIMenu menuWithChildren:@[[UIMenu menuWithTitle:@"Training" children:training],[UIMenu menuWithTitle:@"Episode 1" children:episode]]];
    __weak GameController *weakSelf = self;
    NSMutableArray *options = [NSMutableArray arrayWithObjects:item(@"Retry",^{WebAction(WEB_RETRY,0);}),
        item(self.muted ? @"Unmute" : @"Mute",^{
            GameController *vc = weakSelf; vc.muted = !vc.muted;
            WebAction(WEB_MUTE,vc.muted); [defaults setBool:vc.muted forKey:@"muted"]; [vc updateMenus];
        }),nil];
    Battle b = self.battle;
    BOOL full = b.level >= 2 && b.level <= 10;
    if (full) {
        [options addObject:item(@"Track marine",^{WebAction(WEB_FOLLOW,0);})];
        [options addObject:item(@"Toggle Doomcam",^{WebAction(WEB_DOOMCAM,0);})];
    }
    self.options.menu = [UIMenu menuWithChildren:options];
    NSMutableArray *picks = [NSMutableArray array];
    for (NSNumber *kind in [self.demons.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        NSString *title = self.demons[kind];
        if (!full && ![title hasPrefix:@"Imp "] && ![title hasPrefix:@"Shotgunner "]) continue;
        [picks addObject:item(title,^{WebAction(WEB_PICK,kind.intValue);})];
    }
    if (full) {
        [picks addObject:item(@"Door / lift · 15 / 20",^{WebAction(WEB_TOOL,1);})];
        [picks addObject:item(@"Barrel · 30",^{WebAction(WEB_TOOL,2);})];
    }
    self.demon.menu = [UIMenu menuWithChildren:picks];
}
- (void)refresh {
    Battle b = self.battle;
    [self.levels setTitle:levelName(b.level) forState:UIControlStateNormal];
    [self.pause setTitle:b.paused ? @"Resume" : @"Pause" forState:UIControlStateNormal];
    self.pause.enabled = !(b.step == 3 || b.step == 4 || (b.step >= 6 && b.step <= 9));
    self.tip.text = briefing(b);
    self.tip.textColor = b.step == 3 || b.step == 7 ? UIColor.systemGreenColor : UIColor.whiteColor;
    NSString *stats = [NSString stringWithFormat:@"%d souls  ·  Marine %d%%",b.souls,MAX(0,b.health)];
    if (b.step == 3 && b.damage >= 0) {
        NSString *key = [NSString stringWithFormat:@"bestBlast.%d",b.level];
        NSInteger best = [NSUserDefaults.standardUserDefaults integerForKey:key];
        stats = [NSString stringWithFormat:@"Blast %d  ·  Best %ld  ·  %d barrel%@",b.damage,(long)best,b.chain,b.chain == 1 ? @"" : @"s"];
    }
    if (b.step == 7 && self.clearTicks >= 0) {
        NSInteger best = [NSUserDefaults.standardUserDefaults integerForKey:[NSString stringWithFormat:@"bestClear.%d",b.level]];
        stats = [NSString stringWithFormat:@"Clear %.1f s  ·  Best %.1f s",self.clearTicks/35.0,best/35.0];
    }
    self.stats.text = self.notice.length && b.step != 3 ? self.notice : stats;
    NSString *action = nil;
    if (b.step == 3) action = b.level == 15 ? @"Play Episode 1" : @"Next lesson";
    if (b.step == 1) action = @"Blow barrel";
    if (b.step == 12) action = @"Slam door";
    if (b.step == 7 && self.nextLevel >= 0) action = @"Next map";
    if (b.step == 4 || b.step == 8 || b.step == 9) action = @"Retry";
    self.primary.hidden = !action; self.primary.enabled = !b.paused;
    [self.primary setTitle:action forState:UIControlStateNormal];
    self.primary.backgroundColor = b.step == 1 && !b.near ? UIColor.systemGrayColor : ember();
    self.demon.hidden = !(b.step == 5 || (b.step == 0 && (b.level == 0 || b.level == 1)));
    self.demon.enabled = !b.paused;
    NSString *tool = self.demons[@(b.kind)] ?: @"Choose demon";
    if (b.tool == 1) tool = self.doorWait ? [NSString stringWithFormat:@"Door ready in %d s",self.doorWait] : @"Door / lift";
    if (b.tool == 2) tool = self.barrelWait ? [NSString stringWithFormat:@"Barrel ready in %d s",self.barrelWait] : @"Barrel";
    [self.demon setTitle:[tool stringByAppendingString:@"  ▾"] forState:UIControlStateNormal];
}
- (void)episodeNext:(int)next ticks:(int)ticks barrel:(int)barrel door:(int)door {
    if (_nextLevel == next && _clearTicks == ticks && _barrelWait == barrel && _doorWait == door) return;
    if (ticks >= 0) {
        NSString *key = [NSString stringWithFormat:@"bestClear.%d",self.battle.level];
        NSNumber *best = [NSUserDefaults.standardUserDefaults objectForKey:key];
        if (!best || ticks < best.intValue) [NSUserDefaults.standardUserDefaults setInteger:ticks forKey:key];
    }
    _nextLevel = next; _clearTicks = ticks; _barrelWait = barrel; _doorWait = door; [self refresh];
}
@end

void TouchStatus(int step,int kind,int souls,int health,int near,int mode,int damage,int level,int paused,int chain) {
    GameController *vc = active; Battle old = vc.battle;
    Battle now = {step,kind,souls,health,near,mode,damage,level,paused,chain};
    if (!memcmp(&old,&now,sizeof(now))) return;
    vc.battle = now;
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    if (old.level != level) [d setInteger:level forKey:@"lastLevel"];
    if (step == 3 && old.step != 3) {
        NSString *key = [NSString stringWithFormat:@"bestBlast.%d",level];
        if (damage > [d integerForKey:key]) [d setInteger:damage forKey:key];
        NSArray *done = [d arrayForKey:@"cleared"] ?: @[];
        if (![done containsObject:@(level)]) [d setObject:[done arrayByAddingObject:@(level)] forKey:@"cleared"];
    }
    if (old.level != level || (step == 3 && old.step != 3)) [vc updateMenus];
    [vc refresh];
}
void TouchEpisodeStatus(int next,int ticks,int barrelCooldown,int doorCooldown) {
    [active episodeNext:next ticks:ticks barrel:barrelCooldown door:doorCooldown];
}
void TouchAddDemon(int kind,const char *name,int cost) {
    NSString *title = [@(name) capitalizedString];
    if ([title isEqualToString:@"Shotgunguy"]) title = @"Shotgunner";
    active.demons[@(kind)] = [NSString stringWithFormat:@"%@ · %d souls",title,cost];
}
void TouchNotice(const char *message) {
    if (![active.notice isEqualToString:@(message)]) { active.notice = @(message); [active refresh]; }
}

@interface GameScene : UIResponder <UIWindowSceneDelegate>
@property (nonatomic, strong) UIWindow *window;
@end
@implementation GameScene
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = active ?: [GameController new]; [self.window makeKeyAndVisible];
}
- (void)sceneWillResignActive:(UIScene *)scene { if (active.initialized) WebAction(WEB_INTERRUPT,0); active.clock.paused = YES; }
- (void)sceneDidBecomeActive:(UIScene *)scene { active.clock.paused = NO; }
@end

@interface GameApp : UIResponder <UIApplicationDelegate>
@end
@implementation GameApp
@end
int main(int argc,char **argv) {
    @autoreleasepool { return UIApplicationMain(argc,argv,nil,NSStringFromClass(GameApp.class)); }
}
