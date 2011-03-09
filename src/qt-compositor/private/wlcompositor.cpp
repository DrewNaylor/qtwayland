/****************************************************************************
**
** This file is part of QtCompositor**
**
** Copyright © 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
**
** Contact:  Nokia Corporation qt-info@nokia.com
**
** You may use this file under the terms of the BSD license as follows:
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**
** Redistributions of source code must retain the above copyright
** notice, this list of conditions and the following disclaimer.
**
** Redistributions in binary form must reproduce the above copyright
** notice, this list of conditions and the following disclaimer in the
** documentation and/or other materials provided with the distribution.
**
** Neither the name of Nokia Corporation and its Subsidiary(-ies) nor the
** names of its contributors may be used to endorse or promote products
** derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
** LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
** OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**
****************************************************************************/

#include "wlcompositor.h"

#include "wlobject.h"
#include "wldisplay.h"
#include "wlshmbuffer.h"
#include "wlsurface.h"
#include "qtcompositor.h"

#include <QApplication>
#include <QDesktopWidget>

#include <QSocketNotifier>
#include <QDebug>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>

#include <wayland-server.h>

#ifdef QT_COMPOSITOR_MESA_EGL
#include "../mesa_egl/mesaeglintegration.h"
#endif

namespace Wayland {

void input_device_attach(struct wl_client *client,
                         struct wl_input_device *device_base,
                         uint32_t time,
                         struct wl_buffer *buffer, int32_t x, int32_t y)
{
    Q_UNUSED(client);
    Q_UNUSED(device_base);
    Q_UNUSED(time);
    Q_UNUSED(buffer);
    Q_UNUSED(x);
    Q_UNUSED(y);

    qDebug() << "Client %p input device attach" << client;
}

const static struct wl_input_device_interface input_device_interface = {
    input_device_attach,
};

void destroy_surface(struct wl_resource *resource, struct wl_client *client)
{
    Q_UNUSED(client);
    Surface *surface = wayland_cast<Surface *>((wl_surface *)resource);
    delete surface;
}

void compositor_create_surface(struct wl_client *client,
			       struct wl_compositor *compositor, uint32_t id)
{
    wayland_cast<Compositor *>(compositor)->createSurface(client, id);
}

const static struct wl_compositor_interface compositor_interface = {
    compositor_create_surface
};

void shell_move(struct wl_client *client,
                struct wl_shell *shell,
                struct wl_surface *surface,
                struct wl_input_device *input_device,
                uint32_t time)
{
    Q_UNUSED(client);
    Q_UNUSED(shell);
    Q_UNUSED(surface);
    Q_UNUSED(input_device);
    Q_UNUSED(time);
    qDebug() << "shellMove";
}

void shell_resize(struct wl_client *client,
                  struct wl_shell *shell,
                  struct wl_surface *surface,
                  struct wl_input_device *input_device,
                  uint32_t time,
                  uint32_t edges)
{
    Q_UNUSED(client);
    Q_UNUSED(shell);
    Q_UNUSED(surface);
    Q_UNUSED(input_device);
    Q_UNUSED(time);
    Q_UNUSED(edges);
    qDebug() << "shellResize";
}

void shell_drag(struct wl_client *client,
                struct wl_shell *shell,
                uint32_t id)
{
    Q_UNUSED(client);
    Q_UNUSED(shell);
    Q_UNUSED(id);
    qDebug() << "shellDrag";
}

void shell_selection(struct wl_client *client,
                     struct wl_shell *shell,
                     uint32_t id)
{
    Q_UNUSED(client);
    Q_UNUSED(shell);
    Q_UNUSED(id);
    qDebug() << "shellSelection";
}

const static struct wl_shell_interface shell_interface = {
    shell_move,
    shell_resize,
    shell_drag,
    shell_selection
};

Compositor::Compositor(WaylandCompositor *qt_compositor)
    : m_display(new Display)
    , m_shm(this)
    , m_current_frame(0)
    , m_last_queued_buf(-1)
    , m_qt_compositor(qt_compositor)
{
#ifdef QT_COMPOSITOR_MESA_EGL
    m_graphics_hw_integration = new MesaEglIntegration(qt_compositor);
#endif

    if (wl_compositor_init(base(), &compositor_interface, m_display->handle())) {
        fprintf(stderr, "Fatal: Error initializing compositor\n");
        exit(EXIT_FAILURE);
    }

    memset(&m_input, 0, sizeof(m_input));

    m_display->addGlobalObject(m_output.base(), &wl_output_interface, 0, output_post_geometry);
    m_display->addGlobalObject(&m_shell, &wl_shell_interface, &shell_interface, 0);
    m_display->addGlobalObject(m_shm.base(), &wl_shm_interface, &shm_interface, 0);

    wl_input_device_init(&m_input, base());
    m_display->addGlobalObject(&m_input.object, &wl_input_device_interface, &input_device_interface, 0);

    if (wl_display_add_socket(m_display->handle(), 0)) {
        fprintf(stderr, "Fatal: Failed to open server socket");
        exit(EXIT_FAILURE);
    }

#ifdef QT_COMPOSITOR_WAYLAND_GL
    m_graphics_hw_integration->intializeHardware(m_display->handle());
#endif //QT_COMPOSITOR_WAYLAND_GL

    m_loop = wl_display_get_event_loop(m_display->handle());

    int fd = wl_event_loop_get_fd(m_loop);

    QSocketNotifier *sockNot = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(sockNot, SIGNAL(activated(int)), this, SLOT(processWaylandEvents()));
}

Compositor::~Compositor()
{
    delete m_display;
}

void Compositor::frameFinished()
{
    wl_display_post_frame(m_display->handle(), currentTimeMsecs());
}

void Compositor::createSurface(struct wl_client *client, int id)
{
    Surface *surface = new Surface(client, this);
    printf("Compositor::createSurface: %p %d\n", client, id);

    addClientResource(client, &surface->base()->resource, id, &wl_surface_interface,
            &surface_interface, destroy_surface);

    m_qt_compositor->surfaceCreated(id);

    m_surfaces << surface;
}

struct wl_client *Compositor::getClientFromWinId(uint winId) const
{
    Surface *surface = getSurfaceFromWinId(winId);
    if (surface)
        return surface->base()->client;

    return 0;
}

Surface *Compositor::getSurfaceFromWinId(uint winId) const
{
    foreach (Surface *surface, m_surfaces) {
        if (surface->id() == winId)
            return surface;
    }

    return 0;
}

QImage Compositor::image(uint winId) const
{
    printf("Compositor::image(%x)\n", winId);
    foreach (Surface *surface, m_surfaces) {
        if (surface->id() == winId) {
            return surface->image();
        }
    }

    return QImage();
}

uint Compositor::currentTimeMsecs() const
{
    //### we throw away the time information
    struct timeval tv;
    int ret = gettimeofday(&tv, 0);
    if (ret == 0)
        return tv.tv_sec*1000 + tv.tv_usec/1000;
    return 0;
}

void Compositor::processWaylandEvents()
{
    int ret = wl_event_loop_dispatch(m_loop, 0);
    if (ret)
        fprintf(stderr, "wl_event_loop_dispatch error: %d\n", ret);
}

void Compositor::surfaceResized(Surface *surface, const QSize &size)
{
    m_qt_compositor->surfaceMapped(surface->id(), QRect(QPoint(), size));
}

void Compositor::surfaceDestroyed(Surface *surface)
{
    m_surfaces.removeOne(surface);
    m_qt_compositor->surfaceDestroyed(surface->id());
}

void Compositor::setInputFocus(uint winId)
{
    Surface *surface = getSurfaceFromWinId(winId);

    wl_surface *base = surface ? surface->base() : 0;

    ulong time = currentTimeMsecs();

    wl_input_device_set_keyboard_focus(&m_input, base, time);
    wl_input_device_set_pointer_focus(&m_input, base, time, 0, 0, 0, 0);
}

void Compositor::surfaceDamaged(Surface *surface, const QRect &rect)
{
    m_qt_compositor->surfaceDamaged(surface->id(), rect);
}

uint32_t toWaylandButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return BTN_LEFT;
    case Qt::RightButton:
        return BTN_RIGHT;
    default:
        return BTN_MIDDLE;
    }
}

void Compositor::sendMousePressEvent(uint winId, int x, int y, Qt::MouseButton button)
{
    wl_client *client = getClientFromWinId(winId);
    if (client) {
        uint32_t time = currentTimeMsecs();
        wl_client_post_event(client, &m_input.object,
                             WL_INPUT_DEVICE_BUTTON, time, toWaylandButton(button), 1);
    }
    sendMouseMoveEvent(winId, x, y);
}

void Compositor::sendMouseReleaseEvent(uint winId, int x, int y, Qt::MouseButton button)
{
    wl_client *client = getClientFromWinId(winId);
    if (client)
        wl_client_post_event(client, &m_input.object,
                             WL_INPUT_DEVICE_BUTTON, currentTimeMsecs(), toWaylandButton(button), 0);
    sendMouseMoveEvent(winId, x, y);
}

void Compositor::sendMouseMoveEvent(uint winId, int x, int y)
{
    wl_client *client = getClientFromWinId(winId);

    if (client) {
        uint32_t time = currentTimeMsecs();
        wl_input_device_set_pointer_focus(&m_input,
            getSurfaceFromWinId(winId)->base(), time, x, y, x, y);
        wl_client_post_event(client, &m_input.object,
             WL_INPUT_DEVICE_MOTION, time, x, y, x, y);
    }
}

void Compositor::sendKeyPressEvent(uint winId, uint code)
{
    wl_client *client = getClientFromWinId(winId);
    if (m_input.keyboard_focus != NULL) {
        uint32_t time = currentTimeMsecs();
        wl_client_post_event(client, &m_input.object,
                             WL_INPUT_DEVICE_KEY, time, code - 8, 1);
    }
}

void Compositor::sendKeyReleaseEvent(uint winId, uint code)
{
    wl_client *client = getClientFromWinId(winId);
    if (m_input.keyboard_focus != NULL) {
        uint32_t time = currentTimeMsecs();
        wl_client_post_event(client, &m_input.object,
                             WL_INPUT_DEVICE_KEY, time, code - 8, 0);
    }
}

QWidget * Compositor::topLevelWidget() const
{
    return m_qt_compositor->topLevelWidget();
}

#ifdef QT_COMPOSITOR_WAYLAND_GL
GraphicsHardwareIntegration * Compositor::graphicsHWIntegration() const
{
    return m_graphics_hw_integration;
}
#endif

}

