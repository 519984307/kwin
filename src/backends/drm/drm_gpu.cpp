/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2020 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "drm_gpu.h"
#include <config-kwin.h>
#include "drm_backend.h"
#include "drm_output.h"
#include "drm_object_connector.h"
#include "drm_object_crtc.h"
#include "abstract_egl_backend.h"
#include "logging.h"
#include "session.h"
#include "renderloop_p.h"
#include "main.h"
#include "drm_pipeline.h"
#include "drm_virtual_output.h"
#include "wayland_server.h"
#include "drm_lease_output.h"

#if HAVE_GBM
#include "egl_gbm_backend.h"
#include <gbm.h>
#include "gbm_dmabuf.h"
#endif
// system
#include <algorithm>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
// drm
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_mode.h>
#include <drm_fourcc.h>
// KWaylandServer
#include "KWaylandServer/drmleasedevice_v1_interface.h"

namespace KWin
{

DrmGpu::DrmGpu(DrmBackend *backend, const QString &devNode, int fd, dev_t deviceId)
    : m_fd(fd)
    , m_deviceId(deviceId)
    , m_devNode(devNode)
    , m_atomicModeSetting(false)
    , m_gbmDevice(nullptr)
    , m_platform(backend)
{
    uint64_t capability = 0;

    if (drmGetCap(fd, DRM_CAP_CURSOR_WIDTH, &capability) == 0) {
        m_cursorSize.setWidth(capability);
    } else {
        m_cursorSize.setWidth(64);
    }

    if (drmGetCap(fd, DRM_CAP_CURSOR_HEIGHT, &capability) == 0) {
        m_cursorSize.setHeight(capability);
    } else {
        m_cursorSize.setHeight(64);
    }

    int ret = drmGetCap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, &capability);
    if (ret == 0 && capability == 1) {
        m_presentationClock = CLOCK_MONOTONIC;
    } else {
        m_presentationClock = CLOCK_REALTIME;
    }

    m_addFB2ModifiersSupported = drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &capability) == 0 && capability == 1;
    qCDebug(KWIN_DRM) << "drmModeAddFB2WithModifiers is" << (m_addFB2ModifiersSupported ? "supported" : "not supported") << "on GPU" << m_devNode;

    // find out if this GPU is using the NVidia proprietary driver
    DrmScopedPointer<drmVersion> version(drmGetVersion(fd));
    m_isNVidia = strstr(version->name, "nvidia-drm");
    m_useEglStreams = m_isNVidia;
#if HAVE_GBM
    m_gbmDevice = gbm_create_device(m_fd);
    bool envVarIsSet = false;
    bool value = qEnvironmentVariableIntValue("KWIN_DRM_FORCE_EGL_STREAMS", &envVarIsSet) != 0;
    if (envVarIsSet) {
        m_useEglStreams = m_isNVidia && value;
    } else if (m_gbmDevice) {
        m_useEglStreams = m_isNVidia && strcmp(gbm_device_get_backend_name(m_gbmDevice), "nvidia") != 0;
    }
#endif

    m_socketNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
    connect(m_socketNotifier, &QSocketNotifier::activated, this, &DrmGpu::dispatchEvents);

    // trying to activate Atomic Mode Setting (this means also Universal Planes)
    static const bool atomicModesetting = !qEnvironmentVariableIsSet("KWIN_DRM_NO_AMS");
    if (atomicModesetting) {
        initDrmResources();
    }

    m_leaseDevice = new KWaylandServer::DrmLeaseDeviceV1Interface(waylandServer()->display(), [this]{
        char *path = drmGetDeviceNameFromFd2(m_fd);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            qCWarning(KWIN_DRM) << "Could not open DRM fd for leasing!" << strerror(errno);
        } else {
            if (drmIsMaster(fd)) {
                if (drmDropMaster(fd) != 0) {
                    close(fd);
                    qCWarning(KWIN_DRM) << "Could not create a non-master DRM fd for leasing!" << strerror(errno);
                    return -1;
                }
            }
        }
        return fd;
    });
    connect(m_leaseDevice, &KWaylandServer::DrmLeaseDeviceV1Interface::leaseRequested, this, &DrmGpu::handleLeaseRequest);
    connect(m_leaseDevice, &KWaylandServer::DrmLeaseDeviceV1Interface::leaseRevoked, this, &DrmGpu::handleLeaseRevoked);
    connect(m_platform->session(), &Session::activeChanged, m_leaseDevice, [this](bool active){
        if (!active) {
            // when we gain drm master we want to update outputs first and only then notify the lease device
            m_leaseDevice->setDrmMaster(active);
        }
    });
}

DrmGpu::~DrmGpu()
{
    const auto leaseOutputs = m_leaseOutputs;
    for (const auto &output : leaseOutputs) {
        removeLeaseOutput(output);
    }
    delete m_leaseDevice;
    waitIdle();
    const auto outputs = m_outputs;
    for (const auto &output : outputs) {
        if (auto drmOutput = qobject_cast<DrmOutput *>(output)) {
            removeOutput(drmOutput);
        } else {
            removeVirtualOutput(dynamic_cast<DrmVirtualOutput*>(output));
        }
    }
    if (m_eglDisplay != EGL_NO_DISPLAY) {
        eglTerminate(m_eglDisplay);
    }
    qDeleteAll(m_crtcs);
    qDeleteAll(m_connectors);
    qDeleteAll(m_planes);
    delete m_socketNotifier;
#if HAVE_GBM
    if (m_gbmDevice) {
        gbm_device_destroy(m_gbmDevice);
    }
#endif
    m_platform->session()->closeRestricted(m_fd);
}

clockid_t DrmGpu::presentationClock() const
{
    return m_presentationClock;
}

void DrmGpu::initDrmResources()
{
    // try atomic mode setting
    if (drmSetClientCap(m_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0) {
        DrmScopedPointer<drmModePlaneRes> planeResources(drmModeGetPlaneResources(m_fd));
        if (planeResources) {
            qCDebug(KWIN_DRM) << "Using Atomic Mode Setting on gpu" << m_devNode;
            qCDebug(KWIN_DRM) << "Number of planes on GPU" << m_devNode << ":" << planeResources->count_planes;
            // create the plane objects
            for (unsigned int i = 0; i < planeResources->count_planes; ++i) {
                DrmScopedPointer<drmModePlane> kplane(drmModeGetPlane(m_fd, planeResources->planes[i]));
                DrmPlane *p = new DrmPlane(this, kplane->plane_id);
                if (p->init()) {
                    m_planes << p;
                } else {
                    delete p;
                }
            }
            if (m_planes.isEmpty()) {
                qCWarning(KWIN_DRM) << "Failed to create any plane. Falling back to legacy mode on GPU " << m_devNode;
                m_atomicModeSetting = false;
            } else {
                m_atomicModeSetting = true;
            }
        } else {
            qCWarning(KWIN_DRM) << "Failed to get plane resources. Falling back to legacy mode on GPU " << m_devNode;
            m_atomicModeSetting = false;
        }
    } else {
        qCWarning(KWIN_DRM) << "drmSetClientCap for Atomic Mode Setting failed. Using legacy mode on GPU" << m_devNode;
        m_atomicModeSetting = false;
    }

    DrmScopedPointer<drmModeRes> resources(drmModeGetResources(m_fd));
    if (!resources) {
        qCCritical(KWIN_DRM) << "drmModeGetResources for getting CRTCs failed on GPU" << m_devNode;
        return;
    }
    auto planes = m_planes;
    for (int i = 0; i < resources->count_crtcs; ++i) {
        DrmPlane *primary = nullptr;
        for (const auto &plane : qAsConst(planes)) {
            if (plane->type() == DrmPlane::TypeIndex::Primary
                && plane->isCrtcSupported(i)) {
                primary = plane;
                if (plane->getProp(DrmPlane::PropertyIndex::CrtcId)->current() == resources->crtcs[i]) {
                    break;
                }
            }
        }
        if (m_atomicModeSetting && !primary) {
            qCWarning(KWIN_DRM) << "Could not find a suitable primary plane for crtc" << resources->crtcs[i];
            continue;
        }
        planes.removeOne(primary);
        auto c = new DrmCrtc(this, resources->crtcs[i], i, primary);
        if (!c->init()) {
            delete c;
            continue;
        }
        m_crtcs << c;
    }
}

bool DrmGpu::updateOutputs()
{
    waitIdle();
    DrmScopedPointer<drmModeRes> resources(drmModeGetResources(m_fd));
    if (!resources) {
        qCWarning(KWIN_DRM) << "drmModeGetResources failed";
        return false;
    }

    // In principle these things are supposed to be detected through the wayland protocol.
    // In practice SteamVR doesn't always behave correctly
    auto lessees = drmModeListLessees(m_fd);
    for (const auto &leaseOutput : qAsConst(m_leaseOutputs)) {
        if (leaseOutput->lease()) {
            bool leaseActive = false;
            for (uint i = 0; i < lessees->count; i++) {
                if (lessees->lessees[i] == leaseOutput->lease()->lesseeId()) {
                    leaseActive = true;
                    break;
                }
            }
            if (!leaseActive) {
                leaseOutput->lease()->deny();
            }
        }
    }

    // check for added and removed connectors
    QVector<DrmConnector *> removedConnectors = m_connectors;
    for (int i = 0; i < resources->count_connectors; ++i) {
        const uint32_t currentConnector = resources->connectors[i];
        auto it = std::find_if(m_connectors.constBegin(), m_connectors.constEnd(), [currentConnector] (DrmConnector *c) { return c->id() == currentConnector; });
        if (it == m_connectors.constEnd()) {
            auto c = new DrmConnector(this, currentConnector);
            if (!c->init() || !c->isConnected()) {
                delete c;
                continue;
            }
            m_connectors << c;
        } else {
            (*it)->updateProperties();
            if ((*it)->isConnected()) {
                removedConnectors.removeOne(*it);
            }
        }
    }
    for (const auto &connector : qAsConst(removedConnectors)) {
        if (auto output = findOutput(connector->id())) {
            removeOutput(output);
        } else if (auto leaseOutput = findLeaseOutput(connector->id())) {
            removeLeaseOutput(leaseOutput);
        }
        m_connectors.removeOne(connector);
        delete connector;
    }

    // find unused and connected connectors
    QVector<DrmConnector *> connectedConnectors;
    for (const auto &conn : qAsConst(m_connectors)) {
        auto output = findOutput(conn->id());
        if (conn->isConnected()) {
            connectedConnectors << conn;
            if (output) {
                output->updateModes();
            }
        } else if (output) {
            removeOutput(output);
        } else if (const auto leaseOutput = findLeaseOutput(conn->id())) {
            removeLeaseOutput(leaseOutput);
        }
    }

    // update crtc properties
    for (const auto &crtc : qAsConst(m_crtcs)) {
        crtc->updateProperties();
    }
    // update plane properties
    for (const auto &plane : qAsConst(m_planes)) {
        plane->updateProperties();
    }

    // stash away current pipelines of active outputs
    QMap<DrmOutput*, DrmPipeline*> oldPipelines;
    for (const auto &output : qAsConst(m_drmOutputs)) {
        if (!output->isEnabled()) {
            // create render resources for findWorkingCombination
            Q_EMIT outputEnabled(output);
        }
        m_pipelines.removeOne(output->pipeline());
        oldPipelines.insert(output, output->pipeline());
        output->setPipeline(nullptr);
    }

    if (m_atomicModeSetting) {
        // sort outputs by being already connected (to any CRTC) so that already working outputs get preferred
        std::sort(connectedConnectors.begin(), connectedConnectors.end(), [](auto c1, auto c2){
            return c1->getProp(DrmConnector::PropertyIndex::CrtcId)->current() > c2->getProp(DrmConnector::PropertyIndex::CrtcId)->current();
        });
    }
    auto connectors = connectedConnectors;
    auto crtcs = m_crtcs;
    // don't touch resources that are leased
    for (const auto &output : qAsConst(m_leaseOutputs)) {
        if (output->lease()) {
            connectors.removeOne(output->pipeline()->connector());
            crtcs.removeOne(output->pipeline()->crtc());
        } else {
            m_pipelines.removeOne(output->pipeline());
        }
    }
    auto config = findWorkingCombination({}, connectors, crtcs);
    if (config.isEmpty() && !connectors.isEmpty()) {
        qCCritical(KWIN_DRM) << "DrmGpu::findWorkingCombination failed to find any functional combinations! Reverting to the old configuration!";
        for (auto it = oldPipelines.begin(); it != oldPipelines.end(); it++) {
            it.value()->setOutput(it.key());
            config << it.value();
        }
        for (const auto &leaseOutput : qAsConst(m_leaseOutputs)) {
            if (!leaseOutput->lease()) {
                config << leaseOutput->pipeline();
            }
        }
    } else {
        for (const auto &pipeline : qAsConst(oldPipelines)) {
            delete pipeline;
        }
    }
    m_pipelines << config;

    for (auto it = config.crbegin(); it != config.crend(); it++) {
        const auto &pipeline = *it;
        auto output = pipeline->output();
        if (pipeline->connector()->isNonDesktop()) {
            if (const auto &leaseOutput = findLeaseOutput(pipeline->connector()->id())) {
                leaseOutput->setPipeline(pipeline);
            } else {
                qCDebug(KWIN_DRM, "New non-desktop output on GPU %s: %s", qPrintable(m_devNode), qPrintable(pipeline->connector()->modelName()));
                m_leaseOutputs << new DrmLeaseOutput(pipeline, m_leaseDevice);
            }
            pipeline->setActive(false);
        } else if (m_outputs.contains(output)) {
            // restore output properties
            if (output->isEnabled()) {
                output->updateTransform(output->transform());
                if (output->dpmsMode() != AbstractWaylandOutput::DpmsMode::On) {
                    pipeline->setActive(false);
                }
            } else {
                pipeline->setActive(false);
                Q_EMIT outputDisabled(output);
            }
        } else {
            qCDebug(KWIN_DRM).nospace() << "New output on GPU " << m_devNode << ": " << pipeline->connector()->modelName();
            if (!output->initCursor(m_cursorSize)) {
                m_platform->setSoftwareCursorForced(true);
            }
            m_outputs << output;
            m_drmOutputs << output;
            Q_EMIT outputAdded(output);
        }
    }

    m_leaseDevice->setDrmMaster(true);
    return true;
}

QVector<DrmPipeline *> DrmGpu::findWorkingCombination(const QVector<DrmPipeline *> &pipelines, QVector<DrmConnector *> connectors, QVector<DrmCrtc *> crtcs)
{
    if (connectors.isEmpty() || crtcs.isEmpty()) {
        // no further pipelines can be added -> test configuration
        if (pipelines.isEmpty() || commitCombination(pipelines)) {
            return pipelines;
        } else {
            return {};
        }
    }
    auto connector = connectors.takeFirst();
    const auto encoders = connector->encoders();

    if (m_atomicModeSetting) {
        // try the crtc that this connector is already connected to first
        std::sort(crtcs.begin(), crtcs.end(), [connector](auto c1, auto c2){
            Q_UNUSED(c2)
            if (connector->getProp(DrmConnector::PropertyIndex::CrtcId)->current() == c1->id()) {
                return true;
            } else {
                return false;
            }
        });
    }

    auto recurse = [this, connector, connectors, crtcs, pipelines] (DrmCrtc *crtc) {
        auto pipeline = new DrmPipeline(this, connector, crtc);
        auto crtcsLeft = crtcs;
        crtcsLeft.removeOne(crtc);
        auto allPipelines = pipelines;
        allPipelines << pipeline;
        auto ret = findWorkingCombination(allPipelines, connectors, crtcsLeft);
        if (ret.isEmpty()) {
            delete pipeline;
        }
        return ret;
    };
    for (const auto &encoderId : encoders) {
        DrmScopedPointer<drmModeEncoder> encoder(drmModeGetEncoder(m_fd, encoderId));
        for (const auto &crtc : qAsConst(crtcs)) {
            if (auto workingPipelines = recurse(crtc); !workingPipelines.isEmpty()) {
                return workingPipelines;
            }
        }
    }
    return {};
}

bool DrmGpu::commitCombination(const QVector<DrmPipeline *> &pipelines)
{
    for (const auto &pipeline : pipelines) {
        auto output = findOutput(pipeline->connector()->id());
        if (output) {
            output->setPipeline(pipeline);
            pipeline->setOutput(output);
        } else if (!pipeline->connector()->isNonDesktop()) {
            output = new DrmOutput(this, pipeline);
            Q_EMIT outputEnabled(output);// create render resources for the test
        }
        pipeline->setup();
    }

    if (DrmPipeline::commitPipelines(pipelines, DrmPipeline::CommitMode::Test)) {
        return true;
    } else {
        for (const auto &pipeline : qAsConst(pipelines)) {
            if (!m_outputs.contains(pipeline->output())) {
                Q_EMIT outputDisabled(pipeline->output());
                delete pipeline->output();
            }
        }
        return false;
    }
}

DrmOutput *DrmGpu::findOutput(quint32 connector)
{
    auto it = std::find_if(m_drmOutputs.constBegin(), m_drmOutputs.constEnd(), [connector] (DrmOutput *o) {
        return o->connector()->id() == connector;
    });
    if (it != m_drmOutputs.constEnd()) {
        return *it;
    }
    return nullptr;
}

void DrmGpu::waitIdle()
{
    m_socketNotifier->setEnabled(false);
    while (true) {
        const bool idle = std::all_of(m_drmOutputs.constBegin(), m_drmOutputs.constEnd(), [](DrmOutput *output){
            return !output->m_pageFlipPending;
        });
        if (idle) {
            break;
        }
        pollfd pfds[1];
        pfds[0].fd = m_fd;
        pfds[0].events = POLLIN;

        const int ready = poll(pfds, 1, 30000);
        if (ready < 0) {
            if (errno != EINTR) {
                qCWarning(KWIN_DRM) << Q_FUNC_INFO << "poll() failed:" << strerror(errno);
                break;
            }
        } else if (ready == 0) {
            qCWarning(KWIN_DRM) << "No drm events for gpu" << m_devNode << "within last 30 seconds";
            break;
        } else {
            dispatchEvents();
        }
    };
    m_socketNotifier->setEnabled(true);
}

static std::chrono::nanoseconds convertTimestamp(const timespec &timestamp)
{
    return std::chrono::seconds(timestamp.tv_sec) + std::chrono::nanoseconds(timestamp.tv_nsec);
}

static std::chrono::nanoseconds convertTimestamp(clockid_t sourceClock, clockid_t targetClock,
                                                 const timespec &timestamp)
{
    if (sourceClock == targetClock) {
        return convertTimestamp(timestamp);
    }

    timespec sourceCurrentTime = {};
    timespec targetCurrentTime = {};

    clock_gettime(sourceClock, &sourceCurrentTime);
    clock_gettime(targetClock, &targetCurrentTime);

    const auto delta = convertTimestamp(sourceCurrentTime) - convertTimestamp(timestamp);
    return convertTimestamp(targetCurrentTime) - delta;
}

static void pageFlipHandler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    Q_UNUSED(fd)
    Q_UNUSED(frame)
    auto backend = dynamic_cast<DrmBackend*>(kwinApp()->platform());
    if (!backend) {
        return;
    }
    auto gpu = backend->findGpuByFd(fd);
    if (!gpu) {
        return;
    }
    auto output = static_cast<DrmOutput *>(data);
    if (!gpu->outputs().contains(output)) {
        // output already got deleted
        return;
    }

    // The static_cast<> here are for a 32-bit environment where
    // sizeof(time_t) == sizeof(unsigned int) == 4 . Putting @p sec
    // into a time_t cuts off the most-significant bit (after the
    // year 2038), similarly long can't hold all the bits of an
    // unsigned multiplication.
    std::chrono::nanoseconds timestamp = convertTimestamp(output->gpu()->presentationClock(),
                                                          CLOCK_MONOTONIC,
                                                          { static_cast<time_t>(sec), static_cast<long>(usec * 1000) });
    if (timestamp == std::chrono::nanoseconds::zero()) {
        qCDebug(KWIN_DRM, "Got invalid timestamp (sec: %u, usec: %u) on output %s",
                sec, usec, qPrintable(output->name()));
        timestamp = std::chrono::steady_clock::now().time_since_epoch();
    }

    output->pageFlipped();
    RenderLoopPrivate *renderLoopPrivate = RenderLoopPrivate::get(output->renderLoop());
    renderLoopPrivate->notifyFrameCompleted(timestamp);
}

void DrmGpu::dispatchEvents()
{
    if (!m_platform->session()->isActive()) {
        return;
    }
    drmEventContext context = {};
    context.version = 2;
    context.page_flip_handler = pageFlipHandler;
    drmHandleEvent(m_fd, &context);
}

void DrmGpu::removeOutput(DrmOutput *output)
{
    qCDebug(KWIN_DRM) << "Removing output" << output;
    m_drmOutputs.removeOne(output);
    m_outputs.removeOne(output);
    Q_EMIT outputRemoved(output);
    auto pipeline = output->m_pipeline;
    delete output;
    m_pipelines.removeOne(pipeline);
    delete pipeline;
}

AbstractEglDrmBackend *DrmGpu::eglBackend() const
{
    return m_eglBackend;
}

void DrmGpu::setEglBackend(AbstractEglDrmBackend *eglBackend)
{
    m_eglBackend = eglBackend;
}

DrmBackend *DrmGpu::platform() const {
    return m_platform;
}

const QVector<DrmPipeline*> DrmGpu::pipelines() const
{
    return m_pipelines;
}

DrmVirtualOutput *DrmGpu::createVirtualOutput(const QString &name, const QSize &size, double scale, VirtualOutputMode mode)
{
    auto output = new DrmVirtualOutput(name, this, size);
    output->setScale(scale);
    output->setPlaceholder(mode == Placeholder);
    m_outputs << output;
    Q_EMIT outputEnabled(output);
    Q_EMIT outputAdded(output);
    return output;
}

void DrmGpu::removeVirtualOutput(DrmVirtualOutput *output)
{
    if (m_outputs.removeOne(output)) {
        Q_EMIT outputRemoved(output);
        delete output;
    }
}

bool DrmGpu::isFormatSupported(uint32_t drmFormat) const
{
    if (!m_atomicModeSetting) {
        return drmFormat == DRM_FORMAT_XRGB8888 || drmFormat == DRM_FORMAT_ARGB8888;
    } else {
        for (const auto &plane : qAsConst(m_planes)) {
            if (plane->type() == DrmPlane::TypeIndex::Primary && !plane->formats().contains(drmFormat)) {
                return false;
            }
        }
        return true;
    }
}

DrmLeaseOutput *DrmGpu::findLeaseOutput(quint32 connector)
{
    auto it = std::find_if(m_leaseOutputs.constBegin(), m_leaseOutputs.constEnd(), [connector] (DrmLeaseOutput *o) {
        return o->pipeline()->connector()->id() == connector;
    });
    if (it != m_leaseOutputs.constEnd()) {
        return *it;
    }
    return nullptr;
}

void DrmGpu::handleLeaseRequest(KWaylandServer::DrmLeaseV1Interface *leaseRequest)
{
    QVector<uint32_t> objects;
    QVector<DrmLeaseOutput*> outputs;
    const auto conns = leaseRequest->connectors();
    for (const auto &connector : conns) {
        auto output = qobject_cast<DrmLeaseOutput*>(connector);
        if (m_leaseOutputs.contains(output) && !output->lease()) {
            output->addLeaseObjects(objects);
            outputs << output;
        }
    }
    uint32_t lesseeId;
    int fd = drmModeCreateLease(m_fd, objects.constData(), objects.count(), 0, &lesseeId);
    if (fd < 0) {
        qCWarning(KWIN_DRM) << "Could not create DRM lease!" << strerror(errno);
        qCWarning(KWIN_DRM, "Tried to lease the following %d resources:", objects.count());
        for (const auto &res : qAsConst(objects)) {
            qCWarning(KWIN_DRM) << res;
        }
        leaseRequest->deny();
    } else {
        qCDebug(KWIN_DRM, "Created lease with leaseFd %d and lesseeId %d for %d resources:", fd, lesseeId, objects.count());
        for (const auto &res : qAsConst(objects)) {
            qCDebug(KWIN_DRM) << res;
        }
        leaseRequest->grant(fd, lesseeId);
        for (const auto &output : qAsConst(outputs)) {
            output->leased(leaseRequest);
        }
    }
}

void DrmGpu::handleLeaseRevoked(KWaylandServer::DrmLeaseV1Interface *lease)
{
    const auto conns = lease->connectors();
    for (const auto &connector : conns) {
        auto output = qobject_cast<DrmLeaseOutput*>(connector);
        if (m_leaseOutputs.contains(output)) {
            output->leaseEnded();
        }
    }
    qCDebug(KWIN_DRM, "Revoking lease with leaseID %d", lease->lesseeId());
    drmModeRevokeLease(m_fd, lease->lesseeId());
}

void DrmGpu::removeLeaseOutput(DrmLeaseOutput *output)
{
    qCDebug(KWIN_DRM) << "Removing leased output" << output;
    m_leaseOutputs.removeOne(output);
    auto pipeline = output->pipeline();
    delete output;
    m_pipelines.removeOne(pipeline);
    delete pipeline;
}

QVector<DrmAbstractOutput*> DrmGpu::outputs() const
{
    return m_outputs;
}

int DrmGpu::fd() const
{
    return m_fd;
}

dev_t DrmGpu::deviceId() const
{
    return m_deviceId;
}

bool DrmGpu::atomicModeSetting() const
{
    return m_atomicModeSetting;
}

bool DrmGpu::useEglStreams() const
{
    return m_useEglStreams;
}

QString DrmGpu::devNode() const
{
    return m_devNode;
}

gbm_device *DrmGpu::gbmDevice() const
{
    return m_gbmDevice;
}

EGLDisplay DrmGpu::eglDisplay() const
{
    return m_eglDisplay;
}

void DrmGpu::setGbmDevice(gbm_device *d)
{
    m_gbmDevice = d;
}

void DrmGpu::setEglDisplay(EGLDisplay display)
{
    m_eglDisplay = display;
}

bool DrmGpu::addFB2ModifiersSupported() const
{
    return m_addFB2ModifiersSupported;
}

bool DrmGpu::isNVidia() const
{
    return m_isNVidia;
}

}