/*
 * Game Genie window: per-game list of codes, each with an on/off switch and a
 * description, saved by the cartridge's CRC.
 */
#import "cheats.h"

@implementation CheatManager {
    NSString *_gameKey, *_gameName;
    NSMutableArray<NSMutableDictionary *> *_codes;   /* {code, desc, on} */
    NSWindow *_win;
    NSTextField *_codeField, *_descField, *_error, *_title;
    NSTableView *_table;
    NSButton *_addBtn, *_removeBtn, *_masterCheck;
}

+ (instancetype)shared {
    static CheatManager *m;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ m = [CheatManager new]; });
    return m;
}

- (NSString *)defaultsKey { return [@"cheats." stringByAppendingString:_gameKey]; }
- (BOOL)masterOn {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    return [ud objectForKey:@"cheatsOn"] ? [ud boolForKey:@"cheatsOn"] : YES;
}

- (int)activeCount {
    int n = 0;
    if (!_gameKey || ![self masterOn]) return 0;
    for (NSDictionary *c in _codes) if ([c[@"on"] boolValue]) n++;
    return n;
}

- (void)loadForGame:(NSString *)key name:(NSString *)name {
    _gameKey = [key copy];
    _gameName = [name copy];
    _codes = [NSMutableArray array];
    if (key)
        for (NSDictionary *d in [[NSUserDefaults standardUserDefaults] arrayForKey:[self defaultsKey]])
            [_codes addObject:[d mutableCopy]];
    [self push];
    [self refresh];
}

- (void)save {
    if (_gameKey) [[NSUserDefaults standardUserDefaults] setObject:_codes forKey:[self defaultsKey]];
}

/* hand the enabled, decodable codes to the emulator */
- (void)push {
    Cheat list[MAX_CHEATS];
    int n = 0;
    if (_gameKey && [self masterOn])
        for (NSDictionary *c in _codes) {
            if (n == MAX_CHEATS) break;
            if ([c[@"on"] boolValue] && gg_decode([c[@"code"] UTF8String], &list[n])) n++;
        }
    if (self.apply) self.apply(list, n);
}

static NSString *effect_text(NSString *code) {
    Cheat c;
    if (!gg_decode(code.UTF8String, &c)) return @"invalid";
    return c.has_compare ? [NSString stringWithFormat:@"$%04X → $%02X if $%02X", c.addr, c.value, c.compare]
                         : [NSString stringWithFormat:@"$%04X → $%02X", c.addr, c.value];
}

/* ---------------------------------------------------------------- window */
- (void)showWindow {
    if (!_win) [self build];
    [self refresh];
    [_win makeKeyAndOrderFront:nil];
    [_win makeFirstResponder:_codeField];
}

- (NSTextField *)label:(NSString *)s frame:(NSRect)r {
    NSTextField *l = [NSTextField labelWithString:s];
    l.frame = r;
    [_win.contentView addSubview:l];
    return l;
}

- (void)build {
    const CGFloat W = 600, H = 420;
    _win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                         backing:NSBackingStoreBuffered defer:NO];
    _win.title = @"Game Genie Codes";
    _win.releasedWhenClosed = NO;
    _win.contentMinSize = NSMakeSize(520, 300);
    NSView *cv = _win.contentView;

    _title = [self label:@"" frame:NSMakeRect(20, H - 34, W - 40, 20)];
    _title.font = [NSFont boldSystemFontOfSize:13];
    _title.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;

    NSTextField *l1 = [self label:@"Code" frame:NSMakeRect(20, H - 66, 40, 20)];
    l1.autoresizingMask = NSViewMinYMargin;
    _codeField = [[NSTextField alloc] initWithFrame:NSMakeRect(60, H - 70, 150, 24)];
    _codeField.placeholderString = @"e.g. SXIOPO";
    _codeField.font = [NSFont userFixedPitchFontOfSize:13];
    _codeField.autoresizingMask = NSViewMinYMargin;
    [cv addSubview:_codeField];
    NSTextField *l2 = [self label:@"Description" frame:NSMakeRect(222, H - 66, 80, 20)];
    l2.autoresizingMask = NSViewMinYMargin;
    _descField = [[NSTextField alloc] initWithFrame:NSMakeRect(302, H - 70, W - 302 - 96, 24)];
    _descField.placeholderString = @"optional, e.g. Infinite lives";
    _descField.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [cv addSubview:_descField];
    _addBtn = [NSButton buttonWithTitle:@"Add" target:self action:@selector(addCode:)];
    _addBtn.frame = NSMakeRect(W - 88, H - 72, 70, 28);
    _addBtn.keyEquivalent = @"\r";
    _addBtn.autoresizingMask = NSViewMinXMargin | NSViewMinYMargin;
    [cv addSubview:_addBtn];

    _error = [self label:@"" frame:NSMakeRect(60, H - 94, W - 80, 18)];
    _error.font = [NSFont systemFontOfSize:11];
    _error.textColor = [NSColor systemRedColor];
    _error.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;

    NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(20, 56, W - 40, H - 156)];
    sv.hasVerticalScroller = YES;
    sv.borderType = NSBezelBorder;
    sv.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    _table = [[NSTableView alloc] initWithFrame:sv.bounds];
    _table.usesAlternatingRowBackgroundColors = YES;
    _table.columnAutoresizingStyle = NSTableViewLastColumnOnlyAutoresizingStyle;
    _table.allowsMultipleSelection = YES;
    NSTableColumn *on = [[NSTableColumn alloc] initWithIdentifier:@"on"];
    on.title = @"On"; on.width = 34; on.minWidth = 34; on.maxWidth = 34;
    NSButtonCell *chk = [NSButtonCell new];
    [chk setButtonType:NSButtonTypeSwitch];
    chk.title = @"";
    on.dataCell = chk;
    NSTableColumn *code = [[NSTableColumn alloc] initWithIdentifier:@"code"];
    code.title = @"Code"; code.width = 100; code.editable = NO;
    ((NSTextFieldCell *)code.dataCell).font = [NSFont userFixedPitchFontOfSize:12];
    NSTableColumn *eff = [[NSTableColumn alloc] initWithIdentifier:@"effect"];
    eff.title = @"Patches"; eff.width = 170; eff.editable = NO;
    ((NSTextFieldCell *)eff.dataCell).font = [NSFont userFixedPitchFontOfSize:11];
    ((NSTextFieldCell *)eff.dataCell).textColor = [NSColor secondaryLabelColor];
    NSTableColumn *desc = [[NSTableColumn alloc] initWithIdentifier:@"desc"];
    desc.title = @"Description (double-click to edit)"; desc.width = 220; desc.editable = YES;
    for (NSTableColumn *c in @[ on, code, eff, desc ]) [_table addTableColumn:c];
    _table.dataSource = self;
    _table.delegate = self;
    sv.documentView = _table;
    [cv addSubview:sv];

    _removeBtn = [NSButton buttonWithTitle:@"Remove" target:self action:@selector(removeCodes:)];
    _removeBtn.frame = NSMakeRect(18, 14, 90, 30);
    [cv addSubview:_removeBtn];
    _masterCheck = [NSButton checkboxWithTitle:@"Enable Game Genie" target:self action:@selector(masterChanged:)];
    _masterCheck.frame = NSMakeRect(124, 18, 170, 22);
    [cv addSubview:_masterCheck];
    NSTextField *hint = [self label:@"Codes are saved for this game. Some take effect only after a reset (⌘R)."
                              frame:NSMakeRect(296, 14, W - 316, 30)];
    hint.font = [NSFont systemFontOfSize:11];
    hint.textColor = [NSColor secondaryLabelColor];
    hint.autoresizingMask = NSViewWidthSizable;
    hint.cell.wraps = YES;
    [_win center];
}

- (void)refresh {
    if (!_win) return;
    BOOL has = _gameKey != nil;
    _title.stringValue = has ? _gameName : @"Open a game to add Game Genie codes";
    for (NSControl *c in @[ _codeField, _descField, _addBtn, _removeBtn, _masterCheck, _table ]) c.enabled = has;
    _masterCheck.state = [self masterOn] ? NSControlStateValueOn : NSControlStateValueOff;
    [_table reloadData];
}

- (void)addCode:(id)sender {
    if (!_gameKey) return;
    NSString *text = [_codeField.stringValue uppercaseString];
    NSArray *parts = [text componentsSeparatedByCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@" ,+;\t\n"]];
    NSMutableArray *bad = [NSMutableArray array];
    int added = 0;
    for (NSString *p in parts) {
        NSString *c = [p stringByReplacingOccurrencesOfString:@"-" withString:@""];
        if (!c.length) continue;
        Cheat tmp;
        if (!gg_decode(c.UTF8String, &tmp)) { [bad addObject:p]; continue; }
        BOOL dup = NO;
        for (NSDictionary *e in _codes) if ([e[@"code"] isEqualToString:c]) dup = YES;
        if (dup) continue;
        [_codes addObject:[@{ @"code": c, @"desc": _descField.stringValue ?: @"", @"on": @YES } mutableCopy]];
        added++;
    }
    if (bad.count)
        _error.stringValue = [NSString stringWithFormat:@"Not a valid Game Genie code: %@ (codes are 6 or 8 letters from A E G I K L N O P S T U V X Y Z)",
                                                        [bad componentsJoinedByString:@", "]];
    else _error.stringValue = @"";
    if (added) {
        _codeField.stringValue = @"";
        _descField.stringValue = @"";
        [self save]; [self push]; [_table reloadData];
    }
    [_win makeFirstResponder:_codeField];
}

- (void)removeCodes:(id)sender {
    NSIndexSet *sel = _table.selectedRowIndexes;
    if (!sel.count) return;
    [_codes removeObjectsAtIndexes:sel];
    [self save]; [self push]; [_table reloadData];
}

- (void)masterChanged:(NSButton *)b {
    [[NSUserDefaults standardUserDefaults] setBool:b.state == NSControlStateValueOn forKey:@"cheatsOn"];
    [self push];
}

/* ---- table */
- (NSInteger)numberOfRowsInTableView:(NSTableView *)t { return (NSInteger)_codes.count; }
- (id)tableView:(NSTableView *)t objectValueForTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    NSDictionary *c = _codes[(NSUInteger)row];
    if ([col.identifier isEqualToString:@"on"]) return c[@"on"];
    if ([col.identifier isEqualToString:@"code"]) return c[@"code"];
    if ([col.identifier isEqualToString:@"effect"]) return effect_text(c[@"code"]);
    return c[@"desc"];
}
- (void)tableView:(NSTableView *)t setObjectValue:(id)v forTableColumn:(NSTableColumn *)col row:(NSInteger)row {
    NSMutableDictionary *c = _codes[(NSUInteger)row];
    if ([col.identifier isEqualToString:@"on"]) { c[@"on"] = @([v boolValue]); [self push]; }
    else if ([col.identifier isEqualToString:@"desc"]) c[@"desc"] = v ?: @"";
    [self save];
}
@end
