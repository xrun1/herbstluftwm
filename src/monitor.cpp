#include <cassert>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <sstream>
#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif /* XINERAMA */

#include "root.h"
#include "globals.h"
#include "ipc-protocol.h"
#include "utils.h"
#include "mouse.h"
#include "hook.h"
#include "layout.h"
#include "tag.h"
#include "ewmh.h"
#include "monitor.h"
#include "settings.h"
#include "stack.h"
#include "client.h"
#include "rectangle.h"
#include "monitormanager.h"
#include "root.h"
#include "clientmanager.h"
#include "tagmanager.h"

#include <vector>

using namespace std;


// module internals:
int g_cur_monitor;
static ::HSStack* g_monitor_stack;
MonitorManager* g_monitors;

void monitor_init() {
    g_cur_monitor = 0;
    g_monitor_stack = stack_create();
}

HSMonitor::HSMonitor(Settings* settings_, MonitorManager* monman_, Rectangle rect_, HSTag* tag_)
    : tag(tag_)
    , tag_previous(tag_)
    , tag_string("tag",
                 std::bind(&HSMonitor::getTagString, this),
                 std::bind(&HSMonitor::setTagString, this, std::placeholders::_1))
    , dirty(true)
    , lock_tag("lock_tag", false) // TODO
    , mouse { 0, 0 }
    , rect(rect_)
    , settings(settings_)
    , monman(monman_)
{
    wireAttributes({
        &index,
        &name,
        &tag_string,
        &pad_up,
        &pad_right,
        &pad_down,
        &pad_left,
        &lock_tag,
    });

    name.setValidator([this] (std::string new_name) {
        if (isdigit(new_name[0])) {
            return std::string("The monitor name may not start with a number");
        }
        if (new_name == name())
            return std::string();
        for (auto m : *monman) {
            if (m->name() == new_name) {
                stringstream output;
                output << "Monitor " << m->index()
                       << " already has the name \""
                       << new_name << "\"";
                return output.str();
            }
        }
        return std::string();
    });

    for (auto i : {&pad_up, &pad_left, &pad_right, &pad_down}) {
        i->setWriteable();
        i->changed().connect(this, &HSMonitor::applyLayout);
    }

    slice = slice_create_monitor(this);
    stacking_window = XCreateSimpleWindow(g_display, g_root,
                                             42, 42, 42, 42, 1, 0, 0);

    stack_insert_slice(g_monitor_stack, slice);
}

HSMonitor::~HSMonitor() {
    stack_remove_slice(g_monitor_stack, slice);
    slice_destroy(slice);
    XDestroyWindow(g_display, stacking_window);
}

std::string HSMonitor::getTagString() {
    return tag->name();
}

std::string HSMonitor::setTagString(std::string new_tag_string) {
    HSTag* new_tag = find_tag(new_tag_string.c_str());
    if (!new_tag) {
        return "no tag named \"" + new_tag_string + "\" exists.";
    }
    if (new_tag == tag) return ""; // nothing to do
    bool success = this->setTag(new_tag);
    if (!success) {
        return "tag \"" + new_tag_string + "\" is already on another monitor";
        /* Note: To change this to tag-swapping between monitors, implement a method
         * MonitorManager::stealTag() that will fetch the corresponding monitor
         * and perform the swap */
    }
    return "to be implemented"; // TODO: implement in setTag()
}

void HSMonitor::setIndexAttribute(unsigned long new_index) {
    index = new_index;
}

int HSMonitor::lock_tag_cmd(Input, Output) {
    lock_tag = true;
    return 0;
}

int HSMonitor::unlock_tag_cmd(Input, Output) {
    lock_tag = false;
    return 0;
}

int HSMonitor::list_padding(Input, Output output) {
    output     << pad_up()
        << " " << pad_right()
        << " " << pad_down()
        << " " << pad_left()
        << "\n";
    return 0;
}

/** Set the tag shown on the monitor.
 * Return false if tag is already shown on another monitor.
 */
// TODO this is the job of monitormanager
bool HSMonitor::setTag(HSTag* new_tag) {
    auto owner = find_monitor_with_tag(new_tag);
    if (!owner || owner != this) {
        // TODO do the work!
        return true;
    }
    return owner == this;
}

// TODO this is the job of monitormanager
void monitor_destroy() {
    g_monitors->clearChildren();
    stack_destroy(g_monitor_stack);
}

void HSMonitor::applyLayout() {
    if (settings->monitors_locked) {
        dirty = true;
        return;
    }
    dirty = false;
    Rectangle cur_rect = rect;
    // apply pad
    // FIXME: why does the following + work for attributes pad_* ?
    cur_rect.x += pad_left();
    cur_rect.width -= (pad_left() + pad_right());
    cur_rect.y += pad_up();
    cur_rect.height -= (pad_up() + pad_down());
    if (!g_settings->smart_frame_surroundings() || tag->frame->isSplit()) {
        // apply frame gap
        cur_rect.x += settings->frame_gap();
        cur_rect.y += settings->frame_gap();
        cur_rect.height -= settings->frame_gap();
        cur_rect.width -= settings->frame_gap();
    }
    restack();
    bool isFocused = get_current_monitor() == this;
    if (isFocused) {
        frame_focus_recursive(tag->frame);
    }
    TilingResult res = tag->frame->computeLayout(cur_rect);
    if (tag->floating) {
        for (auto& p : res.data) {
            p.second.floated = true;
        }
    }
    for (auto& p : res.data) {
        HSClient* c = p.first;
        if (c->fullscreen_()) {
            c->resize_fullscreen(rect, res.focus == c && isFocused);
        } else if (p.second.floated) {
            c->resize_floating(this, res.focus == c && isFocused);
        } else {
            c->resize_tiling(p.second.geometry, res.focus == c && isFocused);
        }
        if (p.second.needsRaise) {
            c->raise();
        }
    }
    if (tag->floating) {
        for (auto& p : res.frames) {
            p.first->hide();
        }
    } else {
        for (auto& p : res.frames) {
            p.first->render(p.second, p.first == res.focused_frame && isFocused);
            p.first->updateVisibility(p.second, p.first == res.focused_frame && isFocused);
        }
    }
    if (isFocused) {
        if (res.focus) {
            Root::get()->clients()->focus = res.focus;
        } else {
            Root::get()->clients()->focus = {};
        }
    }

    // remove all enternotify-events from the event queue that were
    // generated while arranging the clients on this monitor
    drop_enternotify_events();
}

int set_monitor_rects_command(int argc, char** argv, Output output) {
    (void)SHIFT(argc, argv);
    if (argc < 1) {
        return HERBST_NEED_MORE_ARGS;
    }
    RectangleVec templates;
    for (int i = 0; i < argc; i++) {
        templates.push_back(Rectangle::fromStr(argv[i]));
    }
    int status = set_monitor_rects(templates);

    if (status == HERBST_TAG_IN_USE) {
        output << argv[0] << ": There are not enough free tags\n";
    } else if (status == HERBST_INVALID_ARGUMENT) {
        output << argv[0] << ": Need at least one rectangle\n";
    }
    return status;
}

int set_monitor_rects(const RectangleVec &templates) {
    if (templates.empty()) {
        return HERBST_INVALID_ARGUMENT;
    }
    HSTag* tag = nullptr;
    unsigned i;
    for (i = 0; i < std::min(templates.size(), g_monitors->size()); i++) {
        auto m = g_monitors->byIdx(i);
        m->rect = templates[i];
    }
    // add additional monitors
    for (; i < templates.size(); i++) {
        tag = find_unused_tag();
        if (!tag) {
            return HERBST_TAG_IN_USE;
        }
        g_monitors->addMonitor(templates[i], tag);
        tag->frame->setVisibleRecursive(true);
    }
    // remove monitors if there are too much
    while (i < g_monitors->size()) {
        g_monitors->removeMonitor(g_monitors->byIdx(i));
    }
    monitor_update_focus_objects();
    all_monitors_apply_layout();
    return 0;
}

HSMonitor* find_monitor_by_name(char* name) {
    for (auto m : *g_monitors) {
        if (m->name == name)
            return m;
    }
    return nullptr;
}

HSMonitor* string_to_monitor(char* str) {
  return g_monitors->byString(str);
}

int add_monitor_command(int argc, char** argv, Output output) {
    // usage: add_monitor RECTANGLE [TAG [NAME]]
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    auto rect = Rectangle::fromStr(argv[1]);
    HSTag* tag = nullptr;
    char* name = nullptr;
    if (argc == 2 || !strcmp("", argv[2])) {
        tag = find_unused_tag();
        if (!tag) {
            output << argv[0] << ": There are not enough free tags\n";
            return HERBST_TAG_IN_USE;
        }
    }
    else {
        tag = find_tag(argv[2]);
        if (!tag) {
            output << argv[0] << ": The tag \"" << argv[2] << "\" does not exist\n";
            return HERBST_INVALID_ARGUMENT;
        }
    }
    if (find_monitor_with_tag(tag)) {
        output << argv[0] <<
            ": The tag \"" << argv[2] << "\" is already viewed on a monitor\n";
        return HERBST_TAG_IN_USE;
    }
    if (argc > 3) {
        name = argv[3];
        if (isdigit(name[0])) {
            output << argv[0] <<
                ": The monitor name may not start with a number\n";
            return HERBST_INVALID_ARGUMENT;
        }
        if (!strcmp("", name)) {
            output << argv[0] <<
                ": An empty monitor name is not permitted\n";
            return HERBST_INVALID_ARGUMENT;
        }
        if (find_monitor_by_name(name)) {
            output << argv[0] <<
                ": A monitor with the same name already exists\n";
            return HERBST_INVALID_ARGUMENT;
        }
    }
    HSMonitor* monitor = g_monitors->addMonitor(rect, tag);
    if (name) monitor->name = name;
    monitor->applyLayout();
    tag->frame->setVisibleRecursive(true);
    emit_tag_changed(tag, g_monitors->size() - 1);
    drop_enternotify_events();
    return 0;
}


int HSMonitor::move_cmd(Input input, Output output) {
    // usage: move_monitor INDEX RECT [PADUP [PADRIGHT [PADDOWN [PADLEFT]]]]
    // moves monitor with number to RECT
    input.shift();
    if (input.empty()) {
        return HERBST_NEED_MORE_ARGS;
    }
    auto new_rect = Rectangle::fromStr(input.front());
    if (new_rect.width < WINDOW_MIN_WIDTH || new_rect.height < WINDOW_MIN_HEIGHT) {
        output << input.command() << "%s: Rectangle is too small\n";
        return HERBST_INVALID_ARGUMENT;
    }
    // else: just move it:
    this->rect = new_rect;
    input.shift();
    if (!input.empty()) pad_up       = stoi(input.front());
    input.shift();
    if (!input.empty()) pad_right    = stoi(input.front());
    input.shift();
    if (!input.empty()) pad_down     = stoi(input.front());
    input.shift();
    if (!input.empty()) pad_left     = stoi(input.front());
    applyLayout();
    return 0;
}

int rename_monitor_command(int argc, char** argv, Output output) {
    if (argc < 3) {
        return HERBST_NEED_MORE_ARGS;
    }
    HSMonitor* mon = g_monitors->byString(argv[1]);
    if (!mon) {
        output << argv[0] <<
            ": Monitor \"" << argv[1] << "\" not found!\n";
        return HERBST_INVALID_ARGUMENT;
    }
    string error = mon->name.change(argv[2]);
    if (!error.empty()) {
        output << argv[0] << ": " << error << "\n";
        return HERBST_INVALID_ARGUMENT;
    } else {
        return 0;
    }
}

int monitor_rect_command(int argc, char** argv, Output output) {
    // usage: monitor_rect [[-p] INDEX]
    char* monitor_str = nullptr;
    HSMonitor* m = nullptr;
    bool with_pad = false;

    // if monitor is supplied
    if (argc > 1) {
        monitor_str = argv[1];
    }
    // if -p is supplied
    if (argc > 2) {
        monitor_str = argv[2];
        if (!strcmp("-p", argv[1])) {
            with_pad = true;
        } else {
            output << argv[0] <<
                ": Invalid argument \"" << argv[1] << "\"\n";
            return HERBST_INVALID_ARGUMENT;
        }
    }
    // if an index is set
    if (monitor_str) {
        m = string_to_monitor(monitor_str);
        if (!m) {
            output << argv[0] <<
                ": Monitor \"" << monitor_str << "\" not found!\n";
            return HERBST_INVALID_ARGUMENT;
        }
    } else {
        m = get_current_monitor();
    }
    auto rect = m->rect;
    if (with_pad) {
        rect.x += m->pad_left;
        rect.width -= m->pad_left + m->pad_right;
        rect.y += m->pad_up;
        rect.height -= m->pad_up + m->pad_down;
    }
    output << rect.x << " " << rect.y << " " << rect.width << " " << rect.height;
    return 0;
}

int monitor_set_pad_command(int argc, char** argv, Output output) {
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    HSMonitor* monitor = string_to_monitor(argv[1]);
    if (!monitor) {
        output << argv[0] <<
            ": Monitor \"" << argv[1] << "\" not found!\n";
        return HERBST_INVALID_ARGUMENT;
    }
    if (argc > 2 && argv[2][0] != '\0') monitor->pad_up       = atoi(argv[2]);
    if (argc > 3 && argv[3][0] != '\0') monitor->pad_right    = atoi(argv[3]);
    if (argc > 4 && argv[4][0] != '\0') monitor->pad_down     = atoi(argv[4]);
    if (argc > 5 && argv[5][0] != '\0') monitor->pad_left     = atoi(argv[5]);
    monitor->applyLayout();
    return 0;
}

HSMonitor* find_monitor_with_tag(HSTag* tag) {
    for (auto m : *g_monitors) {
        if (m->tag == tag)
            return m;
    }
    return nullptr;
}

HSMonitor* get_current_monitor() {
    return g_monitors->byIdx(g_cur_monitor);
}

void all_monitors_apply_layout() {
    for (auto m : *g_monitors) m->applyLayout();
}

int monitor_set_tag(HSMonitor* monitor, HSTag* tag) {
    HSMonitor* other = find_monitor_with_tag(tag);
    if (monitor == other) {
        // nothing to do
        return 0;
    }
    if (monitor->lock_tag) {
        // If the monitor tag is locked, do not change the tag
        if (other) {
            // but if the tag is already visible, change to the
            // displaying monitor
            monitor_focus_by_index(other->index());
            return 0;
        }
        return 1;
    }
    if (other) {
        if (g_settings->swap_monitors_to_get_tag()) {
            if (other->lock_tag) {
                // the monitor we want to steal the tag from is
                // locked. focus that monitor instead
                monitor_focus_by_index(other->index());
                return 0;
            }
            // swap tags
            other->tag = monitor->tag;
            monitor->tag = tag;
            // reset focus
            frame_focus_recursive(tag->frame);
            /* TODO: find the best order of restacking and layouting */
            other->restack();
            monitor->restack();
            other->applyLayout();
            monitor->applyLayout();
            // discard enternotify-events
            drop_enternotify_events();
            monitor_update_focus_objects();
            ewmh_update_current_desktop();
            emit_tag_changed(other->tag, other->index());
            emit_tag_changed(tag, g_cur_monitor);
        } else {
            // if we are not allowed to steal the tag, then just focus the
            // other monitor
            monitor_focus_by_index(other->index());
        }
        return 0;
    }
    HSTag* old_tag = monitor->tag;
    // save old tag
    monitor->tag_previous = old_tag;
    // 1. show new tag
    monitor->tag = tag;
    // first reset focus and arrange windows
    frame_focus_recursive(tag->frame);
    monitor->restack();
    monitor->lock_frames = true;
    monitor->applyLayout();
    monitor->lock_frames = false;
    // then show them (should reduce flicker)
    tag->frame->setVisibleRecursive(true);
    if (!monitor->tag->floating) {
        // monitor->tag->frame->updateVisibility();
    }
    // 2. hide old tag
    old_tag->frame->setVisibleRecursive(false);
    // focus window just has been shown
    // focus again to give input focus
    frame_focus_recursive(tag->frame);
    // discard enternotify-events
    drop_enternotify_events();
    monitor_update_focus_objects();
    ewmh_update_current_desktop();
    emit_tag_changed(tag, g_cur_monitor);
    return 0;
}

int monitor_set_tag_command(int argc, char** argv, Output output) {
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    HSMonitor* monitor = get_current_monitor();
    HSTag*  tag = find_tag(argv[1]);
    if (monitor && tag) {
        int ret = monitor_set_tag(monitor, tag);
        if (ret != 0) {
            output << argv[0] << ": Could not change tag";
            if (monitor->lock_tag) {
                output << " (monitor " << monitor->index() << " is locked)";
            }
            output << "\n";
        }
        return ret;
    } else {
        output << argv[0] <<
            ": Invalid tag \"" << argv[1] << "\"\n";
        return HERBST_INVALID_ARGUMENT;
    }
}

int monitor_set_tag_by_index_command(int argc, char** argv, Output output) {
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    bool skip_visible = false;
    if (argc >= 3 && !strcmp(argv[2], "--skip-visible")) {
        skip_visible = true;
    }
    HSTag* tag = global_tags->byIndexStr(argv[1], skip_visible);
    if (!tag) {
        output << argv[0] <<
            ": Invalid index \"" << argv[1] << "\"\n";
        return HERBST_INVALID_ARGUMENT;
    }
    int ret = monitor_set_tag(get_current_monitor(), &* tag);
    if (ret != 0) {
        output << argv[0] <<
            ": Could not change tag (maybe monitor is locked?)\n";
    }
    return ret;
}

int monitor_set_previous_tag_command(int argc, char** argv, Output output) {
    if (argc < 1) {
        return HERBST_NEED_MORE_ARGS;
    }
    HSMonitor* monitor = get_current_monitor();
    HSTag*  tag = monitor->tag_previous;
    if (monitor && tag) {
        int ret = monitor_set_tag(monitor, tag);
        if (ret != 0) {
            output << argv[0] <<
                    ": Could not change tag (maybe monitor is locked?)\n";
        }
        return ret;
    } else {
        output << argv[0] <<
                ": Invalid monitor or tag\n";
        return HERBST_INVALID_ARGUMENT;
    }
}

int monitor_focus_command(int argc, char** argv, Output output) {
    if (argc < 2) {
        return HERBST_NEED_MORE_ARGS;
    }
    int new_selection = g_monitors->string_to_monitor_index(argv[1]);
    if (new_selection < 0) {
        output << argv[0] <<
            ": Monitor \"" << argv[1] << "\" not found!\n";
        return HERBST_INVALID_ARGUMENT;
    }
    // really change selection
    monitor_focus_by_index((unsigned)new_selection);
    return 0;
}

int monitor_cycle_command(int argc, char** argv) {
    int delta = 1;
    auto count = g_monitors->size();
    if (argc >= 2) {
        delta = atoi(argv[1]);
    }
    int new_selection = g_cur_monitor + delta; // signed for delta calculations
    // fix range of index
    new_selection %= count;
    new_selection += count;
    new_selection %= count;
    // really change selection
    monitor_focus_by_index((unsigned)new_selection);
    return 0;
}

void monitor_focus_by_index(unsigned new_selection) {
    // clamp to last
    new_selection = std::min((unsigned)g_monitors->size() - 1, new_selection);
    HSMonitor* old = get_current_monitor();
    HSMonitor* monitor = g_monitors->byIdx(new_selection);
    if (old == monitor) {
        // nothing to do
        return;
    }
    // change selection globals
    assert(monitor->tag);
    assert(monitor->tag->frame);
    g_cur_monitor = new_selection;
    frame_focus_recursive(monitor->tag->frame);
    // repaint g_monitors
    old->applyLayout();
    monitor->applyLayout();
    int rx, ry;
    {
        // save old mouse position
        Window win, child;
        int wx, wy;
        unsigned int mask;
        if (True == XQueryPointer(g_display, g_root, &win, &child,
            &rx, &ry, &wx, &wy, &mask)) {
            old->mouse.x = rx - old->rect.x;
            old->mouse.y = ry - old->rect.y;
            old->mouse.x = CLAMP(old->mouse.x, 0, old->rect.width-1);
            old->mouse.y = CLAMP(old->mouse.y, 0, old->rect.height-1);
        }
    }
    // restore position of new monitor
    // but only if mouse pointer is not already on new monitor
    int new_x, new_y;
    if ((monitor->rect.x <= rx) && (rx < monitor->rect.x + monitor->rect.width)
        && (monitor->rect.y <= ry) && (ry < monitor->rect.y + monitor->rect.height)) {
        // mouse already is on new monitor
    } else {
        // If the mouse is located in a gap indicated by
        // mouse_recenter_gap at the outer border of the monitor,
        // recenter the mouse.
        if (std::min(monitor->mouse.x, abs(monitor->mouse.x - (int)monitor->rect.width))
                < g_settings->mouse_recenter_gap()
            || std::min(monitor->mouse.y, abs(monitor->mouse.y - (int)monitor->rect.height))
                < g_settings->mouse_recenter_gap()) {
            monitor->mouse.x = monitor->rect.width / 2;
            monitor->mouse.y = monitor->rect.height / 2;
        }
        new_x = monitor->rect.x + monitor->mouse.x;
        new_y = monitor->rect.y + monitor->mouse.y;
        XWarpPointer(g_display, None, g_root, 0, 0, 0, 0, new_x, new_y);
        // discard all mouse events caused by this pointer movage from the
        // event queue, so the focus really stays in the last focused window on
        // this monitor and doesn't jump to the window hovered by the mouse
        drop_enternotify_events();
    }
    // update objects
    monitor_update_focus_objects();
    // emit hooks
    ewmh_update_current_desktop();
    emit_tag_changed(monitor->tag, new_selection);
}

void monitor_update_focus_objects() {
    g_monitors->focus = g_monitors->byIdx(g_cur_monitor);
    tag_update_focus_objects();
}

int HSMonitor::relativeX(int x_root) {
    return x_root - rect.x - pad_left;
}

int HSMonitor::relativeY(int y_root) {
    return y_root - rect.y - pad_up;
}

HSMonitor* monitor_with_coordinate(int x, int y) {
    for (auto m : *g_monitors) {
        if (m->rect.x + m->pad_left <= x
            && m->rect.x + m->rect.width - m->pad_right > x
            && m->rect.y + m->pad_up <= y
            && m->rect.y + m->rect.height - m->pad_down > y) {
            return &* m;
        }
    }
    return nullptr;
}

// monitor detection using xinerama (if available)
#ifdef XINERAMA
// inspired by dwm's isuniquegeom()
static bool geom_unique(XineramaScreenInfo *unique, size_t n, XineramaScreenInfo *info) {
    while (n--)
        if (unique[n].x_org == info->x_org && unique[n].y_org == info->y_org
        &&  unique[n].width == info->width && unique[n].height == info->height)
            return false;
    return true;
}

// inspired by dwm's updategeom()
bool detect_monitors_xinerama(RectangleVec &ret) {
    int i, j, n;
    if (!XineramaIsActive(g_display)) {
        return false;
    }
    XineramaScreenInfo *info = XineramaQueryScreens(g_display, &n);
    XineramaScreenInfo *unique = g_new(XineramaScreenInfo, n);
    /* only consider unique geometries as separate screens */
    for (i = 0, j = 0; i < n; i++) {
        if (geom_unique(unique, j, &info[i]))
        {
            memcpy(&unique[j++], &info[i], sizeof(XineramaScreenInfo));
        }
    }
    XFree(info);
    n = j;

    RectangleVec monitor_rects(n);
    for (i = 0; i < n; i++) {
        monitor_rects[i].x = unique[i].x_org;
        monitor_rects[i].y = unique[i].y_org;
        monitor_rects[i].width = unique[i].width;
        monitor_rects[i].height = unique[i].height;
    }
    ret.swap(monitor_rects);
    g_free(unique);
    return true;
}
#else  /* XINERAMA */

bool detect_monitors_xinerama(RectangleVec &dest) {
    return false;
}

#endif /* XINERAMA */

// monitor detection that always works: one monitor across the entire screen
bool detect_monitors_simple(RectangleVec &dest) {
    XWindowAttributes attributes;
    XGetWindowAttributes(g_display, g_root, &attributes);
    dest = {{ 0, 0, attributes.width, attributes.height }};
    return true;
}

bool detect_monitors_debug_example(RectangleVec &dest) {
    dest = {{ 0, 0,
              g_screen_width * 2 / 3, g_screen_height * 2 / 3 },
            { g_screen_width / 3, g_screen_height / 3,
              g_screen_width * 2 / 3, g_screen_height * 2 / 3}};
    return true;
}


int detect_monitors_command(int argc, const char **argv, Output output) {
    MonitorDetection detect[] = {
        detect_monitors_xinerama,
        detect_monitors_simple,
        detect_monitors_debug_example, // move up for debugging
    };
    RectangleVec monitor_rects;
    // search for a working monitor detection
    // at least the simple detection must work
    for (int i = 0; i < LENGTH(detect); i++) {
        if (detect[i](monitor_rects)) {
            break;
        }
    }
    assert(!monitor_rects.empty());
    bool list_only = false;
    bool disjoin = true;
    //bool drop_small = true;
    FOR (i,1,argc) {
        if      (!strcmp(argv[i], "-l"))            list_only = true;
        else if (!strcmp(argv[i], "--list"))        list_only = true;
        else if (!strcmp(argv[i], "--no-disjoin"))  disjoin = false;
        // TOOD:
        // else if (!strcmp(argv[i], "--keep-small"))  drop_small = false;
        else {
            output << "detect_monitors: unknown flag \"" << argv[i] << "\"\n";
            return HERBST_INVALID_ARGUMENT;
        }
    }

    int ret = 0;
    if (list_only) {
        for (auto m : monitor_rects) {
            output << m << "\n";
        }
    } else {
        // possibly disjoin them
        if (disjoin) {
            RectList* rl = disjoin_rects(monitor_rects);
            monitor_rects.resize(rectlist_length(rl));
            RectList* cur = rl;
            FOR (i,0,monitor_rects.size()) {
                monitor_rects[i] = cur->rect;
                cur = cur->next;
            }
        }
        // apply it
        ret = set_monitor_rects(monitor_rects);
        if (ret == HERBST_TAG_IN_USE) {
            output << argv[0] << ": There are not enough free tags\n";
        }
    }
    return ret;
}

void monitor_stack_to_window_buf(Window* buf, int len, bool real_clients,
                                 int* remain_len) {
    stack_to_window_buf(g_monitor_stack, buf, len, real_clients, remain_len);
}

HSStack* get_monitor_stack() {
    return g_monitor_stack;
}

int monitor_raise_command(int argc, char** argv, Output output) {
    char* cmd_name = argv[0];
    (void)SHIFT(argc, argv);
    HSMonitor* monitor;
    if (argc >= 1) {
        monitor = string_to_monitor(argv[0]);
        if (!monitor) {
            output << cmd_name << ": Monitor \"" << argv[0] << "\" not found!\n";
            return HERBST_INVALID_ARGUMENT;
        }
    } else {
        monitor = get_current_monitor();
    }
    stack_raise_slide(g_monitor_stack, monitor->slice);
    return 0;
}

void monitor_restack(HSMonitor* monitor) {
    monitor->restack();
}

void HSMonitor::restack() {
    int count = 1 + stack_window_count(tag->stack, false);
    Window* buf = g_new(Window, count);
    buf[0] = stacking_window;
    stack_to_window_buf(tag->stack, buf + 1, count - 1, false, nullptr);
    /* remove a focused fullscreen client */
    HSClient* client = tag->frame->focusedClient();
    if (client && client->fullscreen_) {
        Window win = client->decorationWindow();
        XRaiseWindow(g_display, win);
        int idx = array_find(buf, count, sizeof(*buf), &win);
        assert(idx >= 0);
        count--;
        memmove(buf + idx, buf + idx + 1, sizeof(*buf) * (count - idx));
    }
    XRestackWindows(g_display, buf, count);
    g_free(buf);
}

int shift_to_monitor(int argc, char** argv, Output output) {
    if (argc <= 1) {
        return HERBST_NEED_MORE_ARGS;
    }
    char* monitor_str = argv[1];
    HSMonitor* monitor = string_to_monitor(monitor_str);
    if (!monitor) {
        output << monitor_str << ": Invalid monitor\n";
        return HERBST_INVALID_ARGUMENT;
    }
    global_tags->moveFocusedClient(monitor->tag);
    return 0;
}

void all_monitors_replace_previous_tag(HSTag *old, HSTag *newmon) {
    for (auto m : *g_monitors) {
        if (m->tag_previous == old) {
            m->tag_previous = newmon;
        }
    }
}

void drop_enternotify_events() {
    XEvent ev;
    XSync(g_display, False);
    while(XCheckMaskEvent(g_display, EnterWindowMask, &ev));
}

Rectangle HSMonitor::getFloatingArea() {
    auto m = this;
    auto r = m->rect;
    r.x += m->pad_left;
    r.width -= m->pad_left + m->pad_right;
    r.y += m->pad_up;
    r.height -= m->pad_up + m->pad_down;
    return r;
}

