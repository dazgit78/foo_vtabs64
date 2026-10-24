// Playlist Tabs 64 - a vertical / horizontal playlist tab strip with sorting for foobar2000.
//
// Original work written against the public foobar2000 SDK (x64 and x86).
// Plain Win32 + GDI, no ATL/WTL dependency.
//
// Features: playlists as tabs with item counts, vertical list OR horizontal strip
// (or auto, following the panel's shape), click to switch, drag to reorder,
// inline rename (double-click / F2), new / delete, sort playlists (name A-Z / Z-A
// natural order, by size), smooth scrolling, follows the host's colours and font
// (incl. dark mode).

#include <foobar2000/SDK/foobar2000.h>
#include <foobar2000/SDK/ui_element.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using std::max;
using std::min;

#include <windowsx.h>
#include <shlwapi.h>

#ifdef _MSC_VER
#pragma comment(lib, "shlwapi.lib")
#endif

DECLARE_COMPONENT_VERSION("Playlist Tabs 64", "1.1.0",
	"Playlist tab strip - vertical or horizontal - with drag-to-reorder and playlist sorting.\n"
	"Original implementation.");
VALIDATE_COMPONENT_FILENAME("foo_vtabs64.dll");

namespace {

// {7842442D-9B46-496A-B4A9-8D97A5F7786C}
static const GUID guid_element = { 0x7842442d, 0x9b46, 0x496a, { 0xb4, 0xa9, 0x8d, 0x97, 0xa5, 0xf7, 0x78, 0x6c } };

constexpr UINT_PTR TIMER_SCROLL = 1;
constexpr int kDragThreshold = 5;

static std::wstring W(const char* s) {
	pfc::stringcvt::string_wide_from_utf8 w(s);
	return std::wstring(w.get_ptr());
}
static std::string U(const std::wstring& s) {
	pfc::stringcvt::string_utf8_from_wide u(s.c_str(), s.size());
	return std::string(u.get_ptr());
}

// ---------------------------------------------------------------------------
// settings (stored as a tiny text blob in the element configuration)
// ---------------------------------------------------------------------------
enum Orientation { OrientVertical = 0, OrientHorizontal = 1, OrientAuto = 2 };

struct Settings {
	bool showCount = true;
	int orient = OrientVertical;
};
static Settings parseSettings(const ui_element_config::ptr& cfg) {
	Settings s;
	if (cfg.is_valid() && cfg->get_guid() == guid_element && cfg->get_data_size() > 0) {
		std::string t((const char*)cfg->get_data(), cfg->get_data_size());
		if (t.find("showcount=0") != std::string::npos) s.showCount = false;
		if (t.find("orientation=h") != std::string::npos) s.orient = OrientHorizontal;
		else if (t.find("orientation=a") != std::string::npos) s.orient = OrientAuto;
	}
	return s;
}
static ui_element_config::ptr makeConfig(const Settings& s) {
	const char* o = s.orient == OrientHorizontal ? "h" : s.orient == OrientAuto ? "a" : "v";
	std::string t = std::string("showcount=") + (s.showCount ? "1" : "0") + "\norientation=" + o + "\n";
	return ui_element_config::g_create(guid_element, t.data(), t.size());
}

class vt_instance;

class vt_playlist_cb : public playlist_callback_impl_base {
public:
	explicit vt_playlist_cb(vt_instance* owner);
	void on_items_added(t_size, t_size, metadb_handle_list_cref, const bit_array&) override;
	void on_items_removed(t_size, const bit_array&, t_size, t_size) override;
	void on_playlist_activate(t_size, t_size) override;
	void on_playlist_created(t_size, const char*, t_size) override;
	void on_playlists_reorder(const t_size*, t_size) override;
	void on_playlists_removed(const bit_array&, t_size, t_size) override;
	void on_playlist_renamed(t_size, const char*, t_size) override;
	void on_playlist_locked(t_size, bool) override;
	vt_instance* o;
};

class vt_play_cb : public play_callback_impl_base {
public:
	explicit vt_play_cb(vt_instance* owner);
	void on_playback_new_track(metadb_handle_ptr) override;
	void on_playback_stop(play_control::t_stop_reason) override;
	vt_instance* o;
};

enum SortMode { SortNameAsc, SortNameDesc, SortSizeDesc, SortSizeAsc };

class vt_instance : public ui_element_instance {
public:
	vt_instance(HWND parent, ui_element_config::ptr cfg, ui_element_instance_callback_ptr cb)
		: m_callback(cb), m_settings(parseSettings(cfg)) {
		registerClass();
		m_hwnd = CreateWindowEx(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
			0, 0, 0, 0, parent, nullptr, core_api::get_my_instance(), this);
	}
	~vt_instance() {
		if (m_hwnd && IsWindow(m_hwnd)) DestroyWindow(m_hwnd);
		teardown();
	}

	fb2k::hwnd_t get_wnd() override { return m_hwnd; }
	void set_configuration(ui_element_config::ptr cfg) override {
		m_settings = parseSettings(cfg);
		if (m_hwnd) relayout();
	}
	ui_element_config::ptr get_configuration() override { return makeConfig(m_settings); }
	GUID get_guid() override { return guid_element; }
	GUID get_subclass() override { return ui_element_subclass_utility; }
	void set_default_focus() override { if (m_hwnd) SetFocus(m_hwnd); }
	void notify(const GUID& what, t_size, const void*, t_size) override {
		if (what == ui_element_notify_colors_changed || what == ui_element_notify_font_changed) {
			refreshStyle();
			relayout();
		}
	}

	// callback glue
	void cbChanged() { relayout(); }
	void cbRepaint() { if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE); }
	void cbActivated() {
		relayout();
		ensureVisible(playlist_manager::get()->get_active_playlist());
	}

private:
	static constexpr const wchar_t* kClass = L"PlaylistTabs64";

	static void registerClass() {
		static bool done = false;
		if (done) return;
		WNDCLASS wc = {};
		wc.style = CS_DBLCLKS;
		wc.lpfnWndProc = &vt_instance::WndProc;
		wc.hInstance = core_api::get_my_instance();
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.lpszClassName = kClass;
		RegisterClass(&wc);
		done = true;
	}
	static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
		vt_instance* self;
		if (m == WM_NCCREATE) {
			self = (vt_instance*)((CREATESTRUCT*)l)->lpCreateParams;
			SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)self);
			self->m_hwnd = h;
		} else {
			self = (vt_instance*)GetWindowLongPtr(h, GWLP_USERDATA);
		}
		return self ? self->handle(h, m, w, l) : DefWindowProc(h, m, w, l);
	}

	void teardown() {
		m_plcb.reset();
		m_playcb.reset();
		if (m_font) { DeleteObject(m_font); m_font = nullptr; }
		if (m_fontBold) { DeleteObject(m_fontBold); m_fontBold = nullptr; }
	}

	LRESULT handle(HWND h, UINT m, WPARAM w, LPARAM l) {
		switch (m) {
		case WM_CREATE:
			refreshStyle();
			m_plcb.reset(new vt_playlist_cb(this));
			m_playcb.reset(new vt_play_cb(this));
			relayout();
			return 0;
		case WM_DESTROY:
			KillTimer(h, TIMER_SCROLL);
			teardown();
			return 0;
		case WM_NCDESTROY:
			SetWindowLongPtr(h, GWLP_USERDATA, 0);
			m_hwnd = nullptr;
			break;
		case WM_ERASEBKGND: return 1;
		case WM_PAINT: onPaint(); return 0;
		case WM_SIZE: relayout(); return 0;
		case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
		case WM_SETFOCUS:
		case WM_KILLFOCUS:
			InvalidateRect(h, nullptr, FALSE);
			return 0;
		case WM_MOUSEWHEEL: {
			UINT lines = 3;
			SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
			if (lines == WHEEL_PAGESCROLL) lines = 10;
			scrollTo(m_target - (double)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * lines * m_rowH);
			return 0;
		}
		case WM_VSCROLL: if (!m_horiz) onScrollCmd(SB_VERT, LOWORD(w)); return 0;
		case WM_HSCROLL: if (m_horiz) onScrollCmd(SB_HORZ, LOWORD(w)); return 0;
		case WM_TIMER:
			if (w == TIMER_SCROLL) stepScroll();
			return 0;
		case WM_MOUSEMOVE: onMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
		case WM_MOUSELEAVE:
			m_tracking = false;
			m_hover = SIZE_MAX;
			InvalidateRect(h, nullptr, FALSE);
			return 0;
		case WM_LBUTTONDOWN: onLButtonDown(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
		case WM_LBUTTONUP: onLButtonUp(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
		case WM_CAPTURECHANGED:
			if (m_drag != DragNone) { m_drag = DragNone; InvalidateRect(h, nullptr, FALSE); }
			return 0;
		case WM_LBUTTONDBLCLK: {
			size_t r = rowAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
			if (r < m_count) beginRename(r);
			else newPlaylist();
			return 0;
		}
		case WM_MBUTTONUP: {
			size_t r = rowAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
			if (r < m_count) deletePlaylist(r);
			return 0;
		}
		case WM_RBUTTONDOWN: {
			SetFocus(h);
			size_t r = rowAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
			if (r < m_count) playlist_manager::get()->set_active_playlist(r);
			return 0;
		}
		case WM_CONTEXTMENU: {
			POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
			if (pt.x == -1 && pt.y == -1) { pt = { 10, 10 }; ClientToScreen(h, &pt); }
			POINT cp = pt;
			ScreenToClient(h, &cp);
			showContextMenu(pt, rowAt(cp.x, cp.y));
			return 0;
		}
		case WM_KEYDOWN:
			if (onKey((UINT)w)) return 0;
			break;
		}
		return DefWindowProc(h, m, w, l);
	}

	// ---- style ----
	void refreshStyle() {
		if (!m_hwnd) return;
		m_bg = m_callback->query_std_color(ui_color_background);
		m_fg = m_callback->query_std_color(ui_color_text);
		m_selBg = m_callback->query_std_color(ui_color_selection);
		m_hl = m_callback->query_std_color(ui_color_highlight);
		HFONT base = m_callback->query_font_ex(ui_font_tabs);
		if (!base) base = m_callback->query_font_ex(ui_font_default);
		LOGFONT lf = {};
		if (!base || !GetObject(base, sizeof(lf), &lf)) GetObject(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
		if (m_font) DeleteObject(m_font);
		if (m_fontBold) DeleteObject(m_fontBold);
		m_font = CreateFontIndirect(&lf);
		LOGFONT b = lf;
		b.lfWeight = FW_BOLD;
		m_fontBold = CreateFontIndirect(&b);
		HDC dc = GetDC(m_hwnd);
		HGDIOBJ old = SelectObject(dc, m_font);
		TEXTMETRIC tm = {};
		GetTextMetrics(dc, &tm);
		SelectObject(dc, old);
		ReleaseDC(m_hwnd, dc);
		m_pad = max(4, (int)(tm.tmHeight / 3));
		m_rowH = tm.tmHeight + 2 * m_pad;
	}

	// ---- orientation / layout ----
	// m_pos[i] .. m_pos[i+1] is tab i's extent along the main axis (y when vertical, x when horizontal).
	int stripH() const { return m_rowH + 2; }

	void updateOrientation() {
		RECT wr;
		GetWindowRect(m_hwnd, &wr); // window rect (not client) so scrollbars cannot flip the decision
		bool h = m_settings.orient == OrientHorizontal ||
			(m_settings.orient == OrientAuto && (wr.right - wr.left) > (wr.bottom - wr.top));
		if (h != m_horiz) {
			m_horiz = h;
			m_scroll = m_target = 0;
			KillTimer(m_hwnd, TIMER_SCROLL);
		}
	}

	std::wstring tabName(playlist_manager::ptr pm, size_t i) const {
		pfc::string8 nm;
		pm->playlist_get_name(i, nm);
		std::wstring s = W(nm.c_str());
		if (pm->playlist_lock_is_present(i)) s += L"  \x25AA";
		return s;
	}
	std::wstring countText(playlist_manager::ptr pm, size_t i) const {
		wchar_t cnt[32];
		swprintf(cnt, 32, L"%zu", (size_t)pm->playlist_get_item_count(i));
		return cnt;
	}

	void measure(playlist_manager::ptr pm) {
		m_pos.assign(m_count + 1, 0);
		if (!m_horiz) {
			for (size_t i = 0; i <= m_count; ++i) m_pos[i] = (int)(i * m_rowH);
			return;
		}
		HDC dc = GetDC(m_hwnd);
		HGDIOBJ old = SelectObject(dc, m_fontBold); // bold is the widest case
		const int minW = m_rowH * 3, maxW = m_rowH * 12;
		for (size_t i = 0; i < m_count; ++i) {
			std::wstring name = tabName(pm, i);
			SIZE ns = {}, cs = {};
			GetTextExtentPoint32(dc, name.c_str(), (int)name.size(), &ns);
			int w = ns.cx + 2 * m_pad + 4;
			if (m_settings.showCount) {
				std::wstring c = countText(pm, i);
				GetTextExtentPoint32(dc, c.c_str(), (int)c.size(), &cs);
				w += cs.cx + m_pad;
			}
			m_pos[i + 1] = m_pos[i] + std::clamp(w, minW, maxW);
		}
		SelectObject(dc, old);
		ReleaseDC(m_hwnd, dc);
	}

	void relayout() {
		if (!m_hwnd || m_inLayout) return;
		m_inLayout = true;
		auto pm = playlist_manager::get();
		m_count = pm->get_playlist_count();
		updateOrientation();
		measure(pm);
		m_target = std::clamp(m_target, 0.0, maxScroll());
		m_scroll = std::clamp(m_scroll, 0.0, maxScroll());
		updateScrollbars();
		m_inLayout = false;
		InvalidateRect(m_hwnd, nullptr, FALSE);
	}

	int viewLen() const {
		RECT rc;
		GetClientRect(m_hwnd, &rc);
		return m_horiz ? rc.right : rc.bottom;
	}
	int totalLen() const { return m_pos.empty() ? 0 : m_pos.back(); }
	double maxScroll() const { return max(0, totalLen() - viewLen()); }

	void updateScrollbars() {
		ShowScrollBar(m_hwnd, m_horiz ? SB_VERT : SB_HORZ, FALSE);
		SCROLLINFO si = { sizeof(si) };
		si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
		si.nMin = 0;
		si.nMax = max(0, totalLen() - 1);
		si.nPage = (UINT)max(0, viewLen());
		si.nPos = (int)m_scroll;
		SetScrollInfo(m_hwnd, m_horiz ? SB_HORZ : SB_VERT, &si, TRUE);
	}

	// ---- smooth scrolling ----
	void scrollTo(double pos) {
		m_target = std::clamp(pos, 0.0, maxScroll());
		SetTimer(m_hwnd, TIMER_SCROLL, 15, nullptr);
	}
	void stepScroll() {
		double diff = m_target - m_scroll;
		if (fabs(diff) < 0.75) { m_scroll = m_target; KillTimer(m_hwnd, TIMER_SCROLL); }
		else m_scroll += diff * 0.28;
		updateScrollbars();
		InvalidateRect(m_hwnd, nullptr, FALSE);
	}
	void onScrollCmd(int bar, int code) {
		SCROLLINFO si = { sizeof(si) };
		si.fMask = SIF_ALL;
		GetScrollInfo(m_hwnd, bar, &si);
		double pos = m_target;
		switch (code) {
		case SB_LINEUP: pos -= m_rowH; break;
		case SB_LINEDOWN: pos += m_rowH; break;
		case SB_PAGEUP: pos -= si.nPage; break;
		case SB_PAGEDOWN: pos += si.nPage; break;
		case SB_TOP: pos = 0; break;
		case SB_BOTTOM: pos = maxScroll(); break;
		case SB_THUMBTRACK:
		case SB_THUMBPOSITION:
			m_target = m_scroll = std::clamp((double)si.nTrackPos, 0.0, maxScroll());
			updateScrollbars();
			InvalidateRect(m_hwnd, nullptr, FALSE);
			return;
		default: return;
		}
		scrollTo(pos);
	}
	void ensureVisible(size_t i) {
		if (!m_hwnd || i >= m_count || m_pos.size() < m_count + 1) return;
		double top = m_pos[i], bot = m_pos[i + 1], len = viewLen();
		if (top < m_target) scrollTo(top);
		else if (bot > m_target + len) scrollTo(bot - len);
	}

	// ---- hit testing ----
	size_t rowAt(int x, int y) const {
		if (m_count == 0 || m_pos.size() < m_count + 1) return SIZE_MAX;
		if (m_horiz && (y < 0 || y >= stripH())) return SIZE_MAX;
		double c = (m_horiz ? x : y) + m_scroll;
		if (c < 0) return SIZE_MAX;
		size_t i = std::upper_bound(m_pos.begin(), m_pos.end(), (int)c) - m_pos.begin(); // first boundary > c
		if (i == 0 || i > m_count) return SIZE_MAX;
		return i - 1;
	}
	// insertion slot 0..count nearest to the pointer (used while dragging)
	size_t slotAt(int x, int y) const {
		if (m_pos.size() < m_count + 1) return 0;
		double c = (m_horiz ? x : y) + m_scroll;
		size_t i = std::upper_bound(m_pos.begin(), m_pos.end(), (int)max(0.0, c)) - m_pos.begin();
		if (i == 0) return 0;
		if (i > m_count) return m_count;
		size_t r = i - 1;
		return c > (m_pos[r] + m_pos[r + 1]) / 2.0 ? r + 1 : r;
	}
	RECT tabRect(size_t i, const RECT& rc) const {
		int a = m_pos[i] - (int)m_scroll, b = m_pos[i + 1] - (int)m_scroll;
		if (m_horiz) return { a, 0, b, min((int)rc.bottom, stripH()) };
		return { 0, a, rc.right, b };
	}

	// ---- mouse ----
	void onLButtonDown(int x, int y) {
		SetFocus(m_hwnd);
		size_t r = rowAt(x, y);
		if (r >= m_count) return;
		playlist_manager::get()->set_active_playlist(r);
		m_drag = DragPending;
		m_dragFrom = r;
		m_dragStart = { x, y };
		SetCapture(m_hwnd);
	}
	void onMouseMove(int x, int y) {
		if (!m_tracking) {
			TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, m_hwnd, 0 };
			TrackMouseEvent(&t);
			m_tracking = true;
		}
		if (m_drag == DragPending && (abs(x - m_dragStart.x) > kDragThreshold || abs(y - m_dragStart.y) > kDragThreshold))
			m_drag = DragActive;
		if (m_drag == DragActive) {
			m_dropSlot = slotAt(x, y);
			// auto-scroll near the ends of the main axis
			int c = m_horiz ? x : y, len = viewLen();
			if (c < m_rowH / 2) scrollTo(m_target - m_rowH);
			else if (c > len - m_rowH / 2) scrollTo(m_target + m_rowH);
			InvalidateRect(m_hwnd, nullptr, FALSE);
			return;
		}
		size_t r = rowAt(x, y);
		if (r != m_hover) {
			m_hover = r;
			InvalidateRect(m_hwnd, nullptr, FALSE);
		}
	}
	void onLButtonUp(int x, int y) {
		DragState was = m_drag;
		m_drag = DragNone;
		if (GetCapture() == m_hwnd) ReleaseCapture();
		if (was == DragActive) moveTab(m_dragFrom, slotAt(x, y));
		InvalidateRect(m_hwnd, nullptr, FALSE);
	}

	// ---- playlist operations ----
	void moveTab(size_t from, size_t slot) {
		if (from >= m_count) return;
		if (slot == from || slot == from + 1) return; // dropped in place
		std::vector<size_t> order(m_count);
		std::iota(order.begin(), order.end(), (size_t)0);
		order.erase(order.begin() + from);
		size_t dst = slot > from ? slot - 1 : slot;
		order.insert(order.begin() + dst, from);
		applyOrder(order, from);
	}
	// order[newPos] = oldPos. Keeps the previously active playlist active.
	void applyOrder(const std::vector<size_t>& order, size_t activeBefore) {
		auto pm = playlist_manager::get();
		std::vector<t_size> o(order.begin(), order.end());
		if (!pm->reorder(o.data(), o.size())) return; // refused (e.g. locked playlists)
		for (size_t i = 0; i < order.size(); ++i)
			if (order[i] == activeBefore) { pm->set_active_playlist(i); break; }
	}

	void sortPlaylists(SortMode mode) {
		auto pm = playlist_manager::get();
		const size_t n = pm->get_playlist_count();
		if (n < 2) return;
		struct Entry { size_t idx; std::wstring name; size_t count; };
		std::vector<Entry> e(n);
		for (size_t i = 0; i < n; ++i) {
			pfc::string8 nm;
			pm->playlist_get_name(i, nm);
			e[i] = { i, W(nm.c_str()), pm->playlist_get_item_count(i) };
		}
		auto byName = [](const Entry& a, const Entry& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()); };
		std::stable_sort(e.begin(), e.end(), [&](const Entry& a, const Entry& b) {
			switch (mode) {
			case SortNameAsc: return byName(a, b) < 0;
			case SortNameDesc: return byName(a, b) > 0;
			case SortSizeDesc: return a.count > b.count;
			case SortSizeAsc: return a.count < b.count;
			}
			return false;
		});
		std::vector<size_t> order(n);
		for (size_t i = 0; i < n; ++i) order[i] = e[i].idx;
		applyOrder(order, pm->get_active_playlist());
	}

	void newPlaylist() {
		auto pm = playlist_manager::get();
		size_t idx = pm->create_playlist_autoname();
		pm->set_active_playlist(idx);
		relayout(); // make sure the new tab has a rectangle before the editor is placed
		beginRename(idx);
	}
	void deletePlaylist(size_t idx) {
		auto pm = playlist_manager::get();
		if (idx >= pm->get_playlist_count() || pm->playlist_lock_is_present(idx)) return;
		pm->remove_playlist_switch(idx);
	}

	// ---- inline rename ----
	void beginRename(size_t idx) {
		if (idx >= m_count || m_pos.size() < m_count + 1) return;
		endRename(false);
		ensureVisible(idx);
		m_scroll = m_target;
		pfc::string8 nm;
		playlist_manager::get()->playlist_get_name(idx, nm);
		RECT rc;
		GetClientRect(m_hwnd, &rc);
		RECT tr = tabRect(idx, rc);
		int x, y, w;
		if (m_horiz) {
			w = max((int)(tr.right - tr.left), m_rowH * 8);
			x = std::clamp((int)tr.left, 0, max(0, (int)rc.right - w));
			y = 1;
		} else {
			x = m_pad / 2;
			w = max(20, (int)rc.right - m_pad);
			y = tr.top + 1;
		}
		m_edit = CreateWindowEx(0, L"EDIT", W(nm.c_str()).c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
			x, y, w, m_rowH - 2, m_hwnd, nullptr, core_api::get_my_instance(), nullptr);
		if (!m_edit) return;
		m_editIdx = idx;
		SendMessage(m_edit, WM_SETFONT, (WPARAM)m_font, TRUE);
		SetWindowLongPtr(m_edit, GWLP_USERDATA, (LONG_PTR)this);
		m_editOldProc = (WNDPROC)SetWindowLongPtr(m_edit, GWLP_WNDPROC, (LONG_PTR)&vt_instance::EditProc);
		SendMessage(m_edit, EM_SETSEL, 0, -1);
		SetFocus(m_edit);
	}
	void endRename(bool commit) {
		if (!m_edit) return;
		HWND e = m_edit;
		m_edit = nullptr; // guard against re-entry from WM_KILLFOCUS
		if (commit) {
			int n = GetWindowTextLength(e);
			std::wstring buf(n + 1, L'\0');
			GetWindowText(e, &buf[0], n + 1);
			buf.resize(n);
			if (!buf.empty()) {
				std::string u = U(buf);
				playlist_manager::get()->playlist_rename(m_editIdx, u.c_str(), u.size());
			}
		}
		DestroyWindow(e);
		if (m_hwnd) SetFocus(m_hwnd);
		relayout();
	}
	static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
		vt_instance* self = (vt_instance*)GetWindowLongPtr(h, GWLP_USERDATA);
		WNDPROC old = self ? self->m_editOldProc : nullptr;
		if (self) {
			if (m == WM_KEYDOWN && w == VK_RETURN) { self->endRename(true); return 0; }
			if (m == WM_KEYDOWN && w == VK_ESCAPE) { self->endRename(false); return 0; }
			if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
			if (m == WM_KILLFOCUS && self->m_edit == h) { self->endRename(true); return 0; }
		}
		return old ? CallWindowProc(old, h, m, w, l) : DefWindowProc(h, m, w, l);
	}

	// ---- keyboard / menu ----
	bool onKey(UINT vk) {
		auto pm = playlist_manager::get();
		size_t act = pm->get_active_playlist();
		bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
		auto go = [&](size_t i) { pm->set_active_playlist(i); ensureVisible(i); };
		switch (vk) {
		case VK_UP:
		case VK_LEFT: if (act != pfc_infinite && act > 0) go(act - 1); return true;
		case VK_DOWN:
		case VK_RIGHT: if (act != pfc_infinite && act + 1 < m_count) go(act + 1); return true;
		case VK_HOME: if (m_count) go(0); return true;
		case VK_END: if (m_count) go(m_count - 1); return true;
		case VK_F2: if (act != pfc_infinite) beginRename(act); return true;
		case VK_DELETE: if (act != pfc_infinite) deletePlaylist(act); return true;
		case 'N': if (ctrl) { newPlaylist(); return true; } break;
		}
		return false;
	}

	void showContextMenu(POINT pt, size_t row) {
		enum { IDNew = 1, IDRename, IDDelete, IDSortAZ, IDSortZA, IDSortBig, IDSortSmall, IDCount, IDVert, IDHoriz, IDAuto };
		HMENU menu = CreatePopupMenu();
		HMENU sort = CreatePopupMenu();
		HMENU orient = CreatePopupMenu();
		AppendMenu(menu, MF_STRING, IDNew, L"New playlist\tCtrl+N");
		UINT rowFlags = row < m_count ? MF_STRING : (MF_STRING | MF_GRAYED);
		AppendMenu(menu, rowFlags, IDRename, L"Rename\tF2");
		AppendMenu(menu, rowFlags, IDDelete, L"Delete\tDel");
		AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
		AppendMenu(sort, MF_STRING, IDSortAZ, L"Name (A to Z, natural order)");
		AppendMenu(sort, MF_STRING, IDSortZA, L"Name (Z to A)");
		AppendMenu(sort, MF_STRING, IDSortBig, L"Size (largest first)");
		AppendMenu(sort, MF_STRING, IDSortSmall, L"Size (smallest first)");
		AppendMenu(menu, MF_POPUP, (UINT_PTR)sort, L"Sort playlists");
		AppendMenu(orient, MF_STRING, IDVert, L"Vertical");
		AppendMenu(orient, MF_STRING, IDHoriz, L"Horizontal");
		AppendMenu(orient, MF_STRING, IDAuto, L"Automatic (follow panel shape)");
		CheckMenuRadioItem(orient, IDVert, IDAuto,
			m_settings.orient == OrientHorizontal ? IDHoriz : m_settings.orient == OrientAuto ? IDAuto : IDVert, MF_BYCOMMAND);
		AppendMenu(menu, MF_POPUP, (UINT_PTR)orient, L"Orientation");
		AppendMenu(menu, MF_STRING | (m_settings.showCount ? MF_CHECKED : 0), IDCount, L"Show item counts");
		int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, m_hwnd, nullptr);
		DestroyMenu(menu); // also destroys the submenus
		switch (cmd) {
		case IDNew: newPlaylist(); break;
		case IDRename: beginRename(row); break;
		case IDDelete: deletePlaylist(row); break;
		case IDSortAZ: sortPlaylists(SortNameAsc); break;
		case IDSortZA: sortPlaylists(SortNameDesc); break;
		case IDSortBig: sortPlaylists(SortSizeDesc); break;
		case IDSortSmall: sortPlaylists(SortSizeAsc); break;
		case IDCount: m_settings.showCount = !m_settings.showCount; relayout(); break;
		case IDVert: m_settings.orient = OrientVertical; relayout(); break;
		case IDHoriz: m_settings.orient = OrientHorizontal; relayout(); break;
		case IDAuto: m_settings.orient = OrientAuto; relayout(); break;
		}
	}

	// ---- painting ----
	static COLORREF blend(COLORREF a, COLORREF b, int pctB) {
		auto mix = [&](int x, int y) { return (x * (100 - pctB) + y * pctB) / 100; };
		return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
	}
	static bool isDark(COLORREF c) {
		return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000 < 128;
	}
	static void fillRect(HDC dc, int l, int t, int r, int b, COLORREF c) {
		RECT rr = { l, t, r, b };
		SetBkColor(dc, c);
		ExtTextOut(dc, 0, 0, ETO_OPAQUE, &rr, nullptr, 0, nullptr);
	}

	void onPaint() {
		PAINTSTRUCT ps;
		HDC wdc = BeginPaint(m_hwnd, &ps);
		RECT rc;
		GetClientRect(m_hwnd, &rc);
		if (rc.right > 0 && rc.bottom > 0) {
			HDC dc = CreateCompatibleDC(wdc);
			HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
			HGDIOBJ oldBmp = SelectObject(dc, bmp);
			paint(dc, rc);
			BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
			SelectObject(dc, oldBmp);
			DeleteObject(bmp);
			DeleteDC(dc);
		}
		EndPaint(m_hwnd, &ps);
	}

	void paint(HDC dc, const RECT& rc) {
		fillRect(dc, 0, 0, rc.right, rc.bottom, m_bg);
		SetBkMode(dc, TRANSPARENT);
		if (m_pos.size() < m_count + 1) return;
		auto pm = playlist_manager::get();
		const size_t active = pm->get_active_playlist();
		const size_t playing = pm->get_playing_playlist();
		const bool focused = GetFocus() == m_hwnd;
		const COLORREF dim = blend(m_fg, m_bg, 45);
		const COLORREF sepCol = blend(m_bg, m_fg, 15);
		const int viewEnd = m_horiz ? rc.right : rc.bottom;

		size_t first = std::upper_bound(m_pos.begin(), m_pos.end(), (int)m_scroll) - m_pos.begin();
		first = first > 0 ? first - 1 : 0;
		for (size_t i = first; i < m_count; ++i) {
			RECT tr = tabRect(i, rc);
			if ((m_horiz ? tr.left : tr.top) >= viewEnd) break;
			const bool isActive = i == active;
			COLORREF textCol = m_fg, countCol = dim;
			if (isActive) {
				COLORREF sel = focused ? m_selBg : blend(m_selBg, m_bg, 45);
				fillRect(dc, tr.left, tr.top, tr.right, tr.bottom, sel);
				textCol = isDark(sel) ? RGB(255, 255, 255) : RGB(0, 0, 0);
				countCol = blend(textCol, sel, 35);
			} else if (i == m_hover && m_drag != DragActive) {
				fillRect(dc, tr.left, tr.top, tr.right, tr.bottom, blend(m_bg, m_fg, 8));
			}
			if (i == playing && !isActive) textCol = m_hl;

			std::wstring name = tabName(pm, i);
			int right = tr.right - m_pad;
			if (m_settings.showCount) {
				std::wstring cnt = countText(pm, i);
				SelectObject(dc, m_font);
				SetTextColor(dc, countCol);
				RECT cr = { tr.left, tr.top, right, tr.bottom };
				DrawText(dc, cnt.c_str(), -1, &cr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
				SIZE sz = {};
				GetTextExtentPoint32(dc, cnt.c_str(), (int)cnt.size(), &sz);
				right -= sz.cx + m_pad;
			}
			SelectObject(dc, (i == playing) ? m_fontBold : m_font);
			SetTextColor(dc, textCol);
			RECT nr = { tr.left + m_pad + 2, tr.top, max((int)tr.left + m_pad + 3, right), tr.bottom };
			DrawText(dc, name.c_str(), (int)name.size(), &nr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);

			if (m_horiz) {
				fillRect(dc, tr.right - 1, tr.top + 3, tr.right, tr.bottom - 3, sepCol);
				if (isActive) fillRect(dc, tr.left, tr.bottom - 2, tr.right, tr.bottom, m_hl);
			}
			if (isActive && focused) {
				RECT fr = tr;
				DrawFocusRect(dc, &fr);
			}
		}
		// drop indicator while dragging
		if (m_drag == DragActive) {
			int p = m_pos[min(m_dropSlot, m_count)] - (int)m_scroll;
			if (m_horiz) fillRect(dc, p - 1, 0, p + 1, min((int)rc.bottom, stripH()), m_hl);
			else fillRect(dc, 0, p - 1, rc.right, p + 1, m_hl);
		}
	}

	enum DragState { DragNone, DragPending, DragActive };

	HWND m_hwnd = nullptr;
	HWND m_edit = nullptr;
	WNDPROC m_editOldProc = nullptr;
	size_t m_editIdx = 0;
	ui_element_instance_callback_ptr m_callback;
	Settings m_settings;
	std::vector<int> m_pos;
	size_t m_count = 0;
	size_t m_hover = SIZE_MAX;
	bool m_tracking = false, m_horiz = false, m_inLayout = false;
	DragState m_drag = DragNone;
	size_t m_dragFrom = 0, m_dropSlot = 0;
	POINT m_dragStart = {};
	double m_scroll = 0, m_target = 0;
	HFONT m_font = nullptr, m_fontBold = nullptr;
	COLORREF m_bg = 0, m_fg = 0, m_selBg = 0, m_hl = 0;
	int m_rowH = 22, m_pad = 6;
	std::unique_ptr<vt_playlist_cb> m_plcb;
	std::unique_ptr<vt_play_cb> m_playcb;
};

// ---- callback glue ----
static const t_uint32 kFlags = playlist_callback::flag_on_items_added | playlist_callback::flag_on_items_removed |
	playlist_callback::flag_on_playlist_activate | playlist_callback::flag_on_playlist_created |
	playlist_callback::flag_on_playlists_reorder | playlist_callback::flag_on_playlists_removed |
	playlist_callback::flag_on_playlist_renamed | playlist_callback::flag_on_playlist_locked;

vt_playlist_cb::vt_playlist_cb(vt_instance* owner) : playlist_callback_impl_base(kFlags), o(owner) {}
// counts, names and lock marks change tab widths in horizontal mode, so these re-measure
void vt_playlist_cb::on_items_added(t_size, t_size, metadb_handle_list_cref, const bit_array&) { o->cbChanged(); }
void vt_playlist_cb::on_items_removed(t_size, const bit_array&, t_size, t_size) { o->cbChanged(); }
void vt_playlist_cb::on_playlist_activate(t_size, t_size) { o->cbActivated(); }
void vt_playlist_cb::on_playlist_created(t_size, const char*, t_size) { o->cbChanged(); }
void vt_playlist_cb::on_playlists_reorder(const t_size*, t_size) { o->cbChanged(); }
void vt_playlist_cb::on_playlists_removed(const bit_array&, t_size, t_size) { o->cbChanged(); }
void vt_playlist_cb::on_playlist_renamed(t_size, const char*, t_size) { o->cbChanged(); }
void vt_playlist_cb::on_playlist_locked(t_size, bool) { o->cbChanged(); }

vt_play_cb::vt_play_cb(vt_instance* owner)
	: play_callback_impl_base(play_callback::flag_on_playback_new_track | play_callback::flag_on_playback_stop), o(owner) {}
void vt_play_cb::on_playback_new_track(metadb_handle_ptr) { o->cbRepaint(); }
void vt_play_cb::on_playback_stop(play_control::t_stop_reason) { o->cbRepaint(); }

// ---- registration ----
class vt_element : public ui_element {
public:
	GUID get_guid() override { return guid_element; }
	GUID get_subclass() override { return ui_element_subclass_utility; }
	void get_name(pfc::string_base& out) override { out = "Playlist Tabs 64"; }
	ui_element_instance_ptr instantiate(fb2k::hwnd_t parent, ui_element_config::ptr cfg, ui_element_instance_callback_ptr cb) override {
		service_ptr_t<vt_instance> inst = new service_impl_t<vt_instance>(parent, cfg, cb);
		return inst;
	}
	ui_element_config::ptr get_default_configuration() override { return makeConfig(Settings()); }
	ui_element_children_enumerator_ptr enumerate_children(ui_element_config::ptr) override { return nullptr; }
	bool get_description(pfc::string_base& out) override {
		out = "Playlist tabs, vertical or horizontal, with drag-to-reorder, rename and sorting.";
		return true;
	}
};
static service_factory_single_t<vt_element> g_vt_element_factory;

} // namespace
