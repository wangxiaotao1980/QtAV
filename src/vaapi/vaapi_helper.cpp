/******************************************************************************
    QtAV:  Media play library based on Qt and FFmpeg
    Copyright (C) 2014-2015 Wang Bin <wbsecg1@gmail.com>

*   This file is part of QtAV

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
******************************************************************************/
#include "vaapi_helper.h"
#include <fcntl.h> //open()
#include <unistd.h> //close()
#include "utils/Logger.h"

namespace QtAV {
namespace OpenGLHelper {
bool isEGL();
}
namespace vaapi {

dll_helper::dll_helper(const QString &soname, int version)
{
    if (version >= 0)
        m_lib.setFileNameAndVersion(soname, version);
    else
        m_lib.setFileName(soname);
    if (m_lib.load()) {
        qDebug("%s loaded", m_lib.fileName().toUtf8().constData());
    } else {
        if (version >= 0) {
            m_lib.setFileName(soname);
            m_lib.load();
        }
    }
    if (!m_lib.isLoaded())
        qDebug("can not load %s: %s", m_lib.fileName().toUtf8().constData(), m_lib.errorString().toUtf8().constData());
}

#define CONCAT(a, b)    CONCAT_(a, b)
#define CONCAT_(a, b)   a##b
#define STRINGIFY(x)    STRINGIFY_(x)
#define STRINGIFY_(x)   #x
#define STRCASEP(p, x)  STRCASE(CONCAT(p, x))
#define STRCASE(x)      case x: return STRINGIFY(x)

/* Return a string representation of a VAProfile */
const char *profileName(VAProfile profile)
{
    switch (profile) {
#define MAP(profile) \
        STRCASEP(VAProfile, profile)
        MAP(MPEG2Simple);
        MAP(MPEG2Main);
        MAP(MPEG4Simple);
        MAP(MPEG4AdvancedSimple);
        MAP(MPEG4Main);
#if VA_CHECK_VERSION(0,32,0)
        MAP(JPEGBaseline);
        MAP(H263Baseline);
        MAP(H264ConstrainedBaseline);
#endif
        MAP(H264Baseline);
        MAP(H264Main);
        MAP(H264High);
        MAP(VC1Simple);
        MAP(VC1Main);
        MAP(VC1Advanced);
#if VA_CHECK_VERSION(0, 38, 0)
        MAP(HEVCMain);
        MAP(HEVCMain10);
        MAP(VP9Profile0);
        MAP(VP8Version0_3); //defined in 0.37
#endif //VA_CHECK_VERSION(0, 38, 0)
#undef MAP
    default:
        break;
    }
    return "";
}

//TODO: use macro template. DEFINE_DL_SYMB(R, NAME, ARG....);
class X11_API : protected dll_helper {
public:
    typedef Display* XOpenDisplay_t(const char* name);
    typedef int XCloseDisplay_t(Display* dpy);
    typedef int XInitThreads_t();
    X11_API(): dll_helper(QString::fromLatin1("X11"),6) {
        fp_XOpenDisplay = (XOpenDisplay_t*)resolve("XOpenDisplay");
        fp_XCloseDisplay = (XCloseDisplay_t*)resolve("XCloseDisplay");
        fp_XInitThreads = (XInitThreads_t*)resolve("XInitThreads");
    }
    Display* XOpenDisplay(const char* name) {
        assert(fp_XOpenDisplay);
        return fp_XOpenDisplay(name);
    }
    int XCloseDisplay(Display* dpy) {
        assert(fp_XCloseDisplay);
        return fp_XCloseDisplay(dpy);
    }
    int XInitThreads() {
        assert(fp_XInitThreads);
        return fp_XInitThreads();
    }
private:
    XOpenDisplay_t* fp_XOpenDisplay;
    XCloseDisplay_t* fp_XCloseDisplay;
    XInitThreads_t* fp_XInitThreads;
};

class NativeDisplayBase {
    Q_DISABLE_COPY(NativeDisplayBase)
public:
    NativeDisplayBase() :m_handle(0) {}
    virtual ~NativeDisplayBase() {}
    virtual bool initialize(const NativeDisplay& display) = 0;
    virtual VADisplay getVADisplay() = 0;
    uintptr_t handle() { return m_handle;}
protected:
    virtual bool acceptValidExternalHandle(const NativeDisplay& display) {
        if (display.handle && display.handle != -1) { //drm can be 0?
            m_handle = display.handle;
            m_selfCreated = false;
            return true;
        }
        return false;
    }
    intptr_t m_handle;
    bool m_selfCreated;
};

class NativeDisplayX11 Q_DECL_FINAL: public NativeDisplayBase, public VAAPI_X11, public X11_API
{
public:
    NativeDisplayX11() :NativeDisplayBase() { }
    ~NativeDisplayX11() {
        if (m_selfCreated && m_handle)
            XCloseDisplay((Display*)(m_handle));
    }
    bool initialize (const NativeDisplay& display) Q_DECL_OVERRIDE {
        assert(display.type == NativeDisplay::X11 || display.type == NativeDisplay::Auto);
        if (acceptValidExternalHandle(display))
            return true;
        qDebug("NativeDisplayX11...............");
        if (!XInitThreads()) {
            qWarning("XInitThreads failed!");
            return false;
        }
        m_handle = (uintptr_t)XOpenDisplay(NULL);
        m_selfCreated = true;
        return !!m_handle;
    }
    VADisplay getVADisplay() Q_DECL_OVERRIDE {
        if (!m_handle)
            return 0;
        if (!VAAPI_X11::isLoaded())
            return 0;
        return vaGetDisplay((Display*)m_handle);
    }
};

#ifndef QT_NO_OPENGL
class NativeDisplayGLX Q_DECL_FINAL: public NativeDisplayBase, public VAAPI_GLX, public X11_API
{
public:
    NativeDisplayGLX() :NativeDisplayBase() { }
    ~NativeDisplayGLX() {
        if (m_selfCreated && m_handle)
            XCloseDisplay((Display*)(m_handle));
    }
    bool initialize (const NativeDisplay& display) Q_DECL_OVERRIDE {
        assert(display.type == NativeDisplay::GLX || display.type == NativeDisplay::Auto);
        if (acceptValidExternalHandle(display))
            return true;
        qDebug("NativeDisplayGLX..............");
        if (!XInitThreads()) {
            qWarning("XInitThreads failed!");
            return false;
        }
        m_handle = (uintptr_t)XOpenDisplay(NULL);
        m_selfCreated = true;
        return !!m_handle;
    }
    VADisplay getVADisplay() Q_DECL_OVERRIDE {
        if (!m_handle)
            return 0;
        if (!VAAPI_GLX::isLoaded())
            return 0;
        return vaGetDisplayGLX((Display*)m_handle);
    }
};
#endif //QT_NO_OPENGL

class NativeDisplayDrm Q_DECL_FINAL: public NativeDisplayBase, public VAAPI_DRM {
  public:
    NativeDisplayDrm() :NativeDisplayBase(){ }
    ~NativeDisplayDrm() {
        if (m_selfCreated && m_handle && m_handle != -1)
            ::close(m_handle);
    }
    bool initialize (const NativeDisplay& display) Q_DECL_OVERRIDE {
        assert(display.type == NativeDisplay::DRM || display.type == NativeDisplay::Auto);
        if (acceptValidExternalHandle(display))
            return true;
        qDebug("NativeDisplayDrm..............");
        // try drmOpen()?
        static const char* drm_dev[] = {
            "/dev/dri/renderD128", // DRM Render-Nodes
            "/dev/dri/card0",
            NULL
        };
        for (int i = 0; drm_dev[i]; ++i) {
            m_handle = ::open(drm_dev[i], O_RDWR);
            if (m_handle < 0)
                continue;
            qDebug("using drm device: %s, handle: %p", drm_dev[i], (void*)m_handle); //drmGetDeviceNameFromFd
            break;
        }
        m_selfCreated = true;
        return m_handle != -1;
    }
    VADisplay getVADisplay() Q_DECL_OVERRIDE {
        if (m_handle == -1)
            return 0;
        if (!VAAPI_DRM::isLoaded())
            return 0;
        return vaGetDisplayDRM(m_handle);
    }
};

class NativeDisplayVADisplay Q_DECL_FINAL: public NativeDisplayBase{
  public:
    NativeDisplayVADisplay() :NativeDisplayBase(){ }
    bool initialize (const NativeDisplay& display) Q_DECL_OVERRIDE {
        assert(display.type == NativeDisplay::VA);
        return acceptValidExternalHandle(display);
    }
    VADisplay getVADisplay() Q_DECL_OVERRIDE { return (VADisplay)m_handle;}
};

// TODO: use display_ptr cache
display_ptr display_t::create(const NativeDisplay &display)
{
    NativeDisplayPtr native;
    switch (display.type) {
    case NativeDisplay::X11:
        native = NativeDisplayPtr(new NativeDisplayX11());
        break;
    case NativeDisplay::DRM:
        native = NativeDisplayPtr(new NativeDisplayDrm());
        break;
    case NativeDisplay::VA:
        native = NativeDisplayPtr(new NativeDisplayVADisplay());
        break;
    case NativeDisplay::GLX:
#ifndef QT_NO_OPENGL
        native = NativeDisplayPtr(new NativeDisplayGLX());
#else
        qWarning("No OpenGL support in Qt");
#endif
        break;
    default:
        break;
    }
    if (!native)
        return display_ptr();
    if (!native->initialize(display))
        return display_ptr();
    VADisplay va = native->getVADisplay();
    int majorVersion, minorVersion;
    VA_ENSURE(vaInitialize(va, &majorVersion, &minorVersion), display_ptr());
    display_ptr d(new display_t());
    d->m_display = va;
    d->m_native = native;
    d->m_type = display.type;
    d->m_major = majorVersion;
    d->m_minor = minorVersion;
    return d;
}

display_t::~display_t()
{
    if (!m_display)
        return;
    bool init_va = false;
#ifndef QT_NO_OPENGL
    // TODO: if drm+egl works, init_va should be true for DRM
    init_va = OpenGLHelper::isEGL() && nativeDisplayType() == NativeDisplay::X11;
#endif
#if defined(WORKAROUND_VATERMINATE_CRASH)
    init_va = true;
#endif
    if (init_va) {
        int mj, mn;
        // FIXME: for libva-xxx we can unload after vaTerminate to avoid crash. But does not work for egl+dma/drm. I really don't know the reason
        qDebug("vaInitialize before terminate. (work around for vaTerminate() crash)");
        VAWARN(vaInitialize(m_display, &mj, &mn));
    }
    qDebug("vaapi: destroy display %p", m_display);
    VAWARN(vaTerminate(m_display)); //FIXME: what about thread?
    m_display = 0;
}

} //namespace vaapi
} //namespace QtAV

