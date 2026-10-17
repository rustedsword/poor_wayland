# Wayland

Wayland is a project to define a protocol for a compositor to talk to
its clients as well as a library implementation of the protocol.  The
compositor can be a standalone display server running on Linux kernel
modesetting and evdev input devices, an X application, or a wayland
client itself.  The clients can be traditional applications, X servers
(rootless or fullscreen) or other display servers.

The wayland protocol is essentially only about input handling and
buffer management.  The compositor receives input events and forwards
them to the relevant client.  The clients creates buffers and renders
into them and notifies the compositor when it needs to redraw.  The
protocol also handles drag and drop, selections, window management and
other interactions that must go through the compositor.  However, the
protocol does not handle rendering, which is one of the features that
makes wayland so simple.  All clients are expected to handle rendering
themselves, typically through cairo or OpenGL.

## poor_wayland

The libraries run on io_uring through [poor_loop](subprojects/poor_loop):

- Nothing blocks. All I/O, closing fds included, goes through the loop.
  Messages are sent when the loop iteration that queued them ends, events and
  requests are dispatched as soon as they arrive. Connecting, listening and
  destroying finish later, in callbacks.
- The connection takes the fds of requests and events, and closes them once
  they are sent, instead of duplicating them.
- The libraries don't allocate. Displays, clients, sockets, globals,
  resources and proxies are structures of the caller, and so are the
  connection buffers and object maps. Objects created for the peer, like
  proxies of server-created objects or server-side registries, come from
  callbacks.
- A display is used from the thread of its loop only: there are no event
  queues, roundtrips or prepare_read. A roundtrip is a wl_display.sync whose
  callback continues the work.
- Every interface has a structure that holds its proxy, so objects can be
  embedded, and requests that create objects take them, e.g.
  `poor_wl_compositor_create_surface(&app->compositor, &app->surface)`.
- On the server, every interface has a structure that holds its resource,
  e.g. `struct poor_wl_surface_resource`, which request handlers and event
  functions take, so an event can't be sent on the wrong kind of object.
- The scanner generates a dispatcher for every interface, which calls the
  listeners and request handlers directly, so there is no libffi.
- Everything has a `poor_` prefix: libpoor_wayland_client,
  libpoor_wayland_server, poor_wayland_scanner, the headers and all C names,
  so it works next to the system libwayland, even in one process. Only the
  protocol on the wire keeps its names.

It needs Linux 6.11 or newer, liburing 2.7 or newer and a C23 compiler.
An installed poor_loop, which must be the latest one, is used if there is
one, otherwise the submodule: `--force-fallback-for=poor_loop` forces it.

[poor_wayland_swapchain](https://github.com/rustedsword/poor_wayland_swapchain)
presents dmabufs on surfaces for GPU rendering and has the example clients.

Building the libraries is fairly simple, they don't have many dependencies:

    $ meson setup build/ --prefix=PREFIX
    $ ninja -C build/ install

where PREFIX is where you want to install the libraries.

See https://wayland.freedesktop.org for documentation.
