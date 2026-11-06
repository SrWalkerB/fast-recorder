#include "recording/RecorderController.h"
#include "recording/AmfBackend.h"
#include "recording/MediaFoundationBackend.h"
#include "recording/NvencBackend.h"

#include <utility>

namespace fastrecord::recording {

namespace {

std::unique_ptr<IRecordingBackend> createBackend(EncoderEngine engine) {
    if (engine == EncoderEngine::Nvenc) {
        return std::make_unique<NvencBackend>();
    }
    if (engine == EncoderEngine::Amf) {
        return std::make_unique<AmfBackend>();
    }
    if (engine == EncoderEngine::Qsv) {
        return std::make_unique<MediaFoundationBackend>(true);
    }
    return std::make_unique<MediaFoundationBackend>();
}

} // namespace

RecorderController::RecorderController(BackendFactory backendFactory)
    : m_backendFactory(std::move(backendFactory)) {
    if (!m_backendFactory) {
        m_backendFactory = createBackend;
    }
}

RecorderController::~RecorderController() = default;

OperationResult RecorderController::start(const RecordingSettings& settings) {
    if (m_state != RecorderState::Idle) {
        return {false, L"Já existe uma gravação em andamento."};
    }

    m_state = RecorderState::Starting;
    RecordingSettings effectiveSettings = settings;
    effectiveSettings.engine = m_engine;

    if (settings.codec == VideoCodec::Av1 &&
        m_engine != EncoderEngine::Nvenc && m_engine != EncoderEngine::Amf) {
        m_state = RecorderState::Idle;
        return {false, L"AV1 nesta versão exige selecionar um encoder NVENC ou AMF compatível."};
    }

    m_backend = m_backendFactory(m_engine);

    if (m_backend == nullptr) {
        m_state = RecorderState::Idle;
        m_lastError = L"Não foi possível criar o backend de gravação.";
        return {false, m_lastError};
    }

    if (!m_backend->start(effectiveSettings, m_lastError)) {
        m_state = RecorderState::Idle;
        return {false, m_lastError};
    }

    m_lastError.clear();
    m_state = RecorderState::Recording;
    return {true, L"Gravação iniciada."};
}

OperationResult RecorderController::pause() {
    if (m_state != RecorderState::Recording) {
        return {false, L"Não há uma gravação ativa para pausar."};
    }

    if (!m_backend->pause(m_lastError)) {
        return {false, m_lastError};
    }
    m_state = RecorderState::Paused;
    return {true, L"Gravação pausada."};
}

OperationResult RecorderController::resume() {
    if (m_state != RecorderState::Paused) {
        return {false, L"A gravação não está pausada."};
    }

    if (!m_backend->resume(m_lastError)) {
        return {false, m_lastError};
    }
    m_state = RecorderState::Recording;
    return {true, L"Gravação retomada."};
}

OperationResult RecorderController::stop() {
    if (m_state != RecorderState::Recording && m_state != RecorderState::Paused) {
        return {false, L"Não há uma gravação ativa."};
    }

    m_state = RecorderState::Stopping;
    if (!m_backend->stop(m_lastError)) {
        m_state = RecorderState::Idle;
        return {false, m_lastError};
    }
    m_state = RecorderState::Idle;
    return {true, L"Gravação finalizada."};
}

std::filesystem::path RecorderController::outputPath() const {
    return m_backend != nullptr ? m_backend->outputPath() : std::filesystem::path{};
}

} // namespace fastrecord::recording
