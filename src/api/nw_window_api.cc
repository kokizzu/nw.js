#include "content/nw/src/api/nw_window_api.h"

#include "base/base64.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/render_widget_host.h"
#include "chrome/browser/devtools/devtools_window.h"
#include "chrome/browser/extensions/devtools_util.h"
#include "components/ui/zoom/zoom_controller.h"
#include "content/nw/src/api/menu/menu.h"
#include "content/nw/src/api/object_manager.h"
#include "content/public/browser/render_view_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "extensions/browser/app_window/app_window_registry.h"
#include "extensions/browser/extension_zoom_request_client.h"
#include "extensions/components/native_app_window/native_app_window_views.h"
#include "extensions/common/error_utils.h"
#include "extensions/common/constants.h"
#include "ui/gfx/codec/jpeg_codec.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/display.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/gfx/screen.h"

#include "content/nw/src/api/nw_current_window_internal.h"

#if defined(OS_WIN)
#include <shobjidl.h>
#include <dwmapi.h>

#include "ui/gfx/canvas.h"
#include "ui/gfx/icon_util.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/platform_font.h"
#include "ui/gfx/win/dpi.h"
#include "ui/views/win/hwnd_util.h"
#endif

#if defined(OS_LINUX)
#include "chrome/browser/ui/libgtk2ui/gtk2_ui.h"
#include "content/nw/src/browser/menubar_view.h"
#include "content/nw/src/browser/browser_view_layout.h"
using nw::BrowserViewLayout;
#endif

#if defined(OS_MACOSX)
#include "content/nw/src/nw_content_mac.h"
#endif

using content::RenderWidgetHost;
using content::RenderWidgetHostView;
using content::WebContents;
using ui_zoom::ZoomController;

using nw::Menu;

#if defined(OS_LINUX)
using nw::MenuBarView;
static void SetDeskopEnvironment() {
  static bool runOnce = false;
  if (runOnce) return;
  runOnce = true;

  scoped_ptr<base::Environment> env(base::Environment::Create());
  std::string name;
  //if (env->GetVar("CHROME_DESKTOP", &name) && !name.empty())
  //  return;

  if (!env->GetVar("NW_DESKTOP", &name) || name.empty())
    name = "nw.desktop";

  env->SetVar("CHROME_DESKTOP", name);
}

#endif

namespace extensions {
namespace {

const char kNoAssociatedAppWindow[] =
    "The context from which the function was called did not have an "
    "associated app window.";
}

static AppWindow* getAppWindow(UIThreadExtensionFunction* func) {
  AppWindowRegistry* registry = AppWindowRegistry::Get(func->browser_context());
  DCHECK(registry);
  content::WebContents* web_contents = func->GetSenderWebContents();
  if (!web_contents) {
    // No need to set an error, since we won't return to the caller anyway if
    // there's no RVH.
    return NULL;
  }
  return registry->GetAppWindowForWebContents(web_contents);
}

#ifdef OS_WIN
static HWND getHWND(AppWindow* window) {
  if (window == NULL) return NULL;
  native_app_window::NativeAppWindowViews* native_app_window_views =
    static_cast<native_app_window::NativeAppWindowViews*>(
    window->GetBaseWindow());
  return views::HWNDForWidget(native_app_window_views->widget()->GetTopLevelWidget());
}
#endif


bool NwCurrentWindowInternalCloseFunction::RunAsync() {
  scoped_ptr<nwapi::nw_current_window_internal::Close::Params> params(
      nwapi::nw_current_window_internal::Close::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params.get());

  bool force = params->force.get() ? *params->force : false;
  AppWindow* window = getAppWindow(this);
  if (force)
    window->GetBaseWindow()->Close();
  else if (window->NWCanClose())
    window->GetBaseWindow()->Close();

  SendResponse(true);
  return true;
}

void NwCurrentWindowInternalShowDevToolsInternalFunction::OnOpened() {
  SendResponse(true);
}

bool NwCurrentWindowInternalShowDevToolsInternalFunction::RunAsync() {
  content::RenderFrameHost* rfh = render_frame_host();
  content::WebContents* web_contents = content::WebContents::FromRenderFrameHost(rfh);
  scoped_refptr<content::DevToolsAgentHost> agent(
      content::DevToolsAgentHost::GetOrCreateFor(web_contents));
  DevToolsWindow::OpenDevToolsWindow(web_contents);
  DevToolsWindow* devtools_window =
      DevToolsWindow::FindDevToolsWindow(agent.get());
  if (devtools_window)
    devtools_window->SetLoadCompletedCallback(base::Bind(&NwCurrentWindowInternalShowDevToolsInternalFunction::OnOpened, this));
  else
    OnOpened();

  return true;
}

bool NwCurrentWindowInternalCloseDevToolsFunction::RunAsync() {
  content::RenderFrameHost* rfh = render_frame_host();
  content::WebContents* web_contents = content::WebContents::FromRenderFrameHost(rfh);
  scoped_refptr<content::DevToolsAgentHost> agent(
      content::DevToolsAgentHost::GetOrCreateFor(web_contents));
  DevToolsWindow* devtools_window =
      DevToolsWindow::FindDevToolsWindow(agent.get());
  if (devtools_window) {
    devtools_window->Close();
  }
  return true;
}

NwCurrentWindowInternalCapturePageInternalFunction::NwCurrentWindowInternalCapturePageInternalFunction() {
}

NwCurrentWindowInternalCapturePageInternalFunction::~NwCurrentWindowInternalCapturePageInternalFunction() {
}

bool NwCurrentWindowInternalCapturePageInternalFunction::RunAsync() {
  EXTENSION_FUNCTION_VALIDATE(args_);

  scoped_ptr<ImageDetails> image_details;
  if (args_->GetSize() > 1) {
    base::Value* spec = NULL;
    EXTENSION_FUNCTION_VALIDATE(args_->Get(1, &spec) && spec);
    image_details = ImageDetails::FromValue(*spec);
  }

  content::RenderFrameHost* rfh = render_frame_host();
  WebContents* contents = content::WebContents::FromRenderFrameHost(rfh);
  if (!contents)
    return false;

  // The default format and quality setting used when encoding jpegs.
  const api::extension_types::ImageFormat kDefaultFormat =
      api::extension_types::IMAGE_FORMAT_JPEG;
  const int kDefaultQuality = 90;

  image_format_ = kDefaultFormat;
  image_quality_ = kDefaultQuality;

  if (image_details) {
    if (image_details->format !=
        api::extension_types::IMAGE_FORMAT_NONE)
      image_format_ = image_details->format;
    if (image_details->quality.get())
      image_quality_ = *image_details->quality;
  }

  // TODO(miu): Account for fullscreen render widget?  http://crbug.com/419878
  RenderWidgetHostView* const view = contents->GetRenderWidgetHostView();
  RenderWidgetHost* const host = view ? view->GetRenderWidgetHost() : nullptr;
  if (!view || !host) {
    OnCaptureFailure(FAILURE_REASON_VIEW_INVISIBLE);
    return false;
  }

  // By default, the requested bitmap size is the view size in screen
  // coordinates.  However, if there's more pixel detail available on the
  // current system, increase the requested bitmap size to capture it all.
  const gfx::Size view_size = view->GetViewBounds().size();
  gfx::Size bitmap_size = view_size;
  const gfx::NativeView native_view = view->GetNativeView();
  gfx::Screen* const screen = gfx::Screen::GetScreenFor(native_view);
  const float scale =
      screen->GetDisplayNearestWindow(native_view).device_scale_factor();
  if (scale > 1.0f)
    bitmap_size = gfx::ScaleToCeiledSize(view_size, scale);

  host->CopyFromBackingStore(
      gfx::Rect(view_size),
      bitmap_size,
      base::Bind(&NwCurrentWindowInternalCapturePageInternalFunction::CopyFromBackingStoreComplete,
                 this),
      kN32_SkColorType);
  return true;
}

void NwCurrentWindowInternalCapturePageInternalFunction::CopyFromBackingStoreComplete(
    const SkBitmap& bitmap,
    content::ReadbackResponse response) {
  if (response == content::READBACK_SUCCESS) {
    OnCaptureSuccess(bitmap);
    return;
  }
  OnCaptureFailure(FAILURE_REASON_UNKNOWN);
}

void NwCurrentWindowInternalCapturePageInternalFunction::OnCaptureSuccess(const SkBitmap& bitmap) {
  std::vector<unsigned char> data;
  SkAutoLockPixels screen_capture_lock(bitmap);
  bool encoded = false;
  std::string mime_type;
  switch (image_format_) {
    case api::extension_types::IMAGE_FORMAT_JPEG:
      encoded = gfx::JPEGCodec::Encode(
          reinterpret_cast<unsigned char*>(bitmap.getAddr32(0, 0)),
          gfx::JPEGCodec::FORMAT_SkBitmap,
          bitmap.width(),
          bitmap.height(),
          static_cast<int>(bitmap.rowBytes()),
          image_quality_,
          &data);
      mime_type = kMimeTypeJpeg;
      break;
    case api::extension_types::IMAGE_FORMAT_PNG:
      encoded =
          gfx::PNGCodec::EncodeBGRASkBitmap(bitmap,
                                            true,  // Discard transparency.
                                            &data);
      mime_type = kMimeTypePng;
      break;
    default:
      NOTREACHED() << "Invalid image format.";
  }

  if (!encoded) {
    OnCaptureFailure(FAILURE_REASON_ENCODING_FAILED);
    return;
  }

  std::string base64_result;
  base::StringPiece stream_as_string(
      reinterpret_cast<const char*>(vector_as_array(&data)), data.size());

  base::Base64Encode(stream_as_string, &base64_result);
  base64_result.insert(
      0, base::StringPrintf("data:%s;base64,", mime_type.c_str()));
  SetResult(new base::StringValue(base64_result));
  SendResponse(true);
}

void NwCurrentWindowInternalCapturePageInternalFunction::OnCaptureFailure(FailureReason reason) {
  const char* reason_description = "internal error";
  switch (reason) {
    case FAILURE_REASON_UNKNOWN:
      reason_description = "unknown error";
      break;
    case FAILURE_REASON_ENCODING_FAILED:
      reason_description = "encoding failed";
      break;
    case FAILURE_REASON_VIEW_INVISIBLE:
      reason_description = "view is invisible";
      break;
  }
  error_ = ErrorUtils::FormatErrorMessage("Failed to capture tab: *",
                                          reason_description);
  SendResponse(false);
}

NwCurrentWindowInternalClearMenuFunction::NwCurrentWindowInternalClearMenuFunction() {
}

NwCurrentWindowInternalClearMenuFunction::~NwCurrentWindowInternalClearMenuFunction() {
}

bool NwCurrentWindowInternalClearMenuFunction::RunAsync() {
  return true;
}

NwCurrentWindowInternalSetMenuFunction::NwCurrentWindowInternalSetMenuFunction() {
}

NwCurrentWindowInternalSetMenuFunction::~NwCurrentWindowInternalSetMenuFunction() {
}

bool NwCurrentWindowInternalSetMenuFunction::RunAsync() {
  int id = 0;
  EXTENSION_FUNCTION_VALIDATE(args_->GetInteger(0, &id));
  AppWindow* window = getAppWindow(this);
  if (!window) {
    error_ = kNoAssociatedAppWindow;
    return false;
  }
  nw::ObjectManager* obj_manager = nw::ObjectManager::Get(browser_context());
  Menu* menu = (Menu*)obj_manager->GetApiObject(id);

  window->menu_ = menu;
#if defined(OS_MACOSX)
  NWChangeAppMenu(menu);
#endif

#if defined(OS_LINUX)
  native_app_window::NativeAppWindowViews* native_app_window_views =
      static_cast<native_app_window::NativeAppWindowViews*>(
          window->GetBaseWindow());

  MenuBarView* menubar = new MenuBarView();
  static_cast<BrowserViewLayout*>(native_app_window_views->GetLayoutManager())->set_menu_bar(menubar);
  native_app_window_views->AddChildView(menubar);
  menubar->UpdateMenu(menu->model());
  native_app_window_views->layout_();
  native_app_window_views->SchedulePaint();
#endif
  // The menu is lazily built.
#if defined(OS_WIN) //FIXME
  menu->Rebuild();
  menu->SetWindow(window);

  native_app_window::NativeAppWindowViews* native_app_window_views =
      static_cast<native_app_window::NativeAppWindowViews*>(
          window->GetBaseWindow());

  // menu is nwapi::Menu, menu->menu_ is NativeMenuWin,
  BOOL ret = ::SetMenu(views::HWNDForWidget(native_app_window_views->widget()->GetTopLevelWidget()), menu->menu_->GetNativeMenu());
  if (!ret)
	  LOG(ERROR) << "error setting menu";

  ::DrawMenuBar(views::HWNDForWidget(native_app_window_views->widget()->GetTopLevelWidget()));
  native_app_window_views->SchedulePaint();
#endif
  //FIXME menu->UpdateKeys( native_app_window_views->widget()->GetFocusManager() );
  return true;
}
  
#if defined(OS_WIN)
static HICON createBadgeIcon(const HWND hWnd, const TCHAR *value, const int sizeX, const int sizeY) {
  // canvas for the overlay icon
  gfx::Canvas canvas(gfx::Size(sizeX, sizeY), 1, false);

  // drawing red circle
  SkPaint paint;
  paint.setColor(SK_ColorRED);
  canvas.DrawCircle(gfx::Point(sizeX / 2, sizeY / 2), sizeX / 2, paint);

  // drawing the text
  gfx::PlatformFont *platform_font = gfx::PlatformFont::CreateDefault();
  const int fontSize = sizeY * 0.65f;
  gfx::Font font(platform_font->GetFontName(), fontSize);
  platform_font->Release();
  platform_font = NULL;
  const int yMargin = (sizeY - fontSize) / 2;
  canvas.DrawStringRectWithFlags(value, gfx::FontList(font), SK_ColorWHITE, gfx::Rect(sizeX, fontSize + yMargin + 1), gfx::Canvas::TEXT_ALIGN_CENTER);

  // return the canvas as windows native icon handle
  return IconUtil::CreateHICONFromSkBitmap(canvas.ExtractImageRep().sk_bitmap());
}
#endif

#ifndef OS_MACOSX
bool NwCurrentWindowInternalSetBadgeLabelFunction::RunAsync() {
  EXTENSION_FUNCTION_VALIDATE(args_);
  std::string badge;
  EXTENSION_FUNCTION_VALIDATE(args_->GetString(0, &badge));
#if defined(OS_WIN)
  base::win::ScopedComPtr<ITaskbarList3> taskbar;
  HRESULT result = taskbar.CreateInstance(CLSID_TaskbarList, NULL,
    CLSCTX_INPROC_SERVER);

  if (FAILED(result)) {
    error_ = "Failed creating a TaskbarList3 object: ";
    LOG(ERROR) << error_ << result;
    return false;
  }

  result = taskbar->HrInit();
  if (FAILED(result)) {
    error_ = "Failed initializing an ITaskbarList3 interface.";
    LOG(ERROR) << error_;
    return false;
  }

  HICON icon = NULL;
  HWND hWnd = getHWND(getAppWindow(this));
  if (hWnd == NULL) {
    error_ = kNoAssociatedAppWindow;
    LOG(ERROR) << error_;
    return false;
  }
  const float scale = gfx::GetDPIScale();
  if (badge.size())
    icon = createBadgeIcon(hWnd, base::UTF8ToUTF16(badge).c_str(), 16 * scale, 16 * scale);

  taskbar->SetOverlayIcon(hWnd, icon, L"Status");
  DestroyIcon(icon);
#elif defined(OS_LINUX)
  views::LinuxUI* linuxUI = views::LinuxUI::instance();
  if (linuxUI == NULL) {
    error_ = "LinuxUI::instance() is NULL";
    return false;
  }
  SetDeskopEnvironment();
  linuxUI->SetDownloadCount(atoi(badge.c_str()));
#else
  error_ = "NwCurrentWindowInternalSetBadgeLabelFunction NOT Implemented"
  NOTIMPLEMENTED() << error_;
  return false;
#endif
  return true;
}
  
bool NwCurrentWindowInternalRequestAttentionFunction::RunAsync() {
  EXTENSION_FUNCTION_VALIDATE(args_);
  int count;
  EXTENSION_FUNCTION_VALIDATE(args_->GetInteger(0, &count));
#if defined(OS_WIN)
  FLASHWINFO fwi;
  fwi.cbSize = sizeof(fwi);
  fwi.hwnd = getHWND(getAppWindow(this));
  if (fwi.hwnd == NULL) {
    error_ = kNoAssociatedAppWindow;
    LOG(ERROR) << error_;
    return false;
  }
  if (count != 0) {
    fwi.dwFlags = FLASHW_ALL;
    fwi.uCount = count < 0 ? 4 : count;
    fwi.dwTimeout = 0;
  }
  else {
    fwi.dwFlags = FLASHW_STOP;
  }
  FlashWindowEx(&fwi);
#elif defined(OS_LINUX)
  AppWindow* window = getAppWindow(this);
  if (!window) {
    error_ = kNoAssociatedAppWindow;
    return false;
  }
  window->GetBaseWindow()->FlashFrame(count);
#else
  error_ = "NwCurrentWindowInternalRequestAttentionFunction NOT Implemented"
  NOTIMPLEMENTED() << error_;
  return false;
#endif
  return true;
}
  
bool NwCurrentWindowInternalSetProgressBarFunction::RunAsync() {
  EXTENSION_FUNCTION_VALIDATE(args_);
  double progress;
  EXTENSION_FUNCTION_VALIDATE(args_->GetDouble(0, &progress));
#if defined(OS_WIN)
  base::win::ScopedComPtr<ITaskbarList3> taskbar;
  HRESULT result = taskbar.CreateInstance(CLSID_TaskbarList, NULL,
    CLSCTX_INPROC_SERVER);

  if (FAILED(result)) {
    error_ = "Failed creating a TaskbarList3 object: ";
    LOG(ERROR) <<  error_ << result;
    return false;
  }

  result = taskbar->HrInit();
  if (FAILED(result)) {
    error_ = "Failed initializing an ITaskbarList3 interface.";
    LOG(ERROR) << error_;
    return false;
  }

  HWND hWnd = getHWND(getAppWindow(this));
  if (hWnd == NULL) {
    error_ = kNoAssociatedAppWindow;
    LOG(ERROR) << error_;
    return false;
  }
  TBPFLAG tbpFlag = TBPF_NOPROGRESS;

  if (progress > 1) {
    tbpFlag = TBPF_INDETERMINATE;
  }
  else if (progress >= 0) {
    tbpFlag = TBPF_NORMAL;
    taskbar->SetProgressValue(hWnd, progress * 100, 100);
  }

  taskbar->SetProgressState(hWnd, tbpFlag);
#elif defined(OS_LINUX)
  views::LinuxUI* linuxUI = views::LinuxUI::instance();
  if (linuxUI == NULL) {
    error_ = "LinuxUI::instance() is NULL";
    return false;
  }
  SetDeskopEnvironment();
  linuxUI->SetProgressFraction(progress);
#else
  error_ = "NwCurrentWindowInternalSetProgressBarFunction NOT Implemented"
  NOTIMPLEMENTED() << error_;
  return false;
#endif
  return true;
}
#endif

bool NwCurrentWindowInternalReloadIgnoringCacheFunction::RunAsync() {
  content::WebContents* web_contents = GetSenderWebContents();
  web_contents->GetController().ReloadIgnoringCache(false);
  SendResponse(true);
  return true;
}

bool NwCurrentWindowInternalGetZoomFunction::RunNWSync(base::ListValue* response, std::string* error) {
  content::WebContents* web_contents = GetSenderWebContents();
  if (!web_contents)
    return false;
  double zoom_level =
      ZoomController::FromWebContents(web_contents)->GetZoomLevel();
  response->AppendDouble(zoom_level);
  return true;
}

bool NwCurrentWindowInternalSetZoomFunction::RunNWSync(base::ListValue* response, std::string* error) {
  double zoom_level;

  EXTENSION_FUNCTION_VALIDATE(args_->GetDouble(0, &zoom_level));
  content::WebContents* web_contents = GetSenderWebContents();
  if (!web_contents)
    return false;
  ZoomController* zoom_controller =
      ZoomController::FromWebContents(web_contents);
  scoped_refptr<ExtensionZoomRequestClient> client(
      new ExtensionZoomRequestClient(extension()));
  if (!zoom_controller->SetZoomLevelByClient(zoom_level, client)) {
    return false;
  }
  return true;
}

bool NwCurrentWindowInternalEnterKioskModeFunction::RunAsync() {
  AppWindow* window = getAppWindow(this);
  window->ForcedFullscreen();
  SendResponse(true);
  return true;
}

bool NwCurrentWindowInternalLeaveKioskModeFunction::RunAsync() {
  AppWindow* window = getAppWindow(this);
  window->Restore();
  SendResponse(true);
  return true;
}

bool NwCurrentWindowInternalToggleKioskModeFunction::RunAsync() {
  AppWindow* window = getAppWindow(this);
  if (window->IsFullscreen() || window->IsForcedFullscreen())
    window->Restore();
  else
    window->ForcedFullscreen();
  SendResponse(true);
  return true;
}

bool NwCurrentWindowInternalIsKioskInternalFunction::RunNWSync(base::ListValue* response, std::string* error) {
  AppWindow* window = getAppWindow(this);
  if (window->IsFullscreen() || window->IsForcedFullscreen())
    response->AppendBoolean(true);
  else
    response->AppendBoolean(false);
  return true;
}


} // namespace extensions

