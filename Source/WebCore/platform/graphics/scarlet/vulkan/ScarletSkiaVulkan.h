// Thread-confined Ganesh device and owned shared GPU surfaces.
#pragma once
#include <cstdint>
#include <memory>
class GrDirectContext;
class SkSurface;
class SkSurfaceProps;
namespace WebCore {
class ScarletSkiaVulkan final : public std::enable_shared_from_this<ScarletSkiaVulkan> {
public:
    class Surface final {
    public:
        ~Surface();
        Surface(const Surface&) = delete;
        Surface& operator=(const Surface&) = delete;
        SkSurface& skSurface() const;
        // Caller owns the duplicated native capability. Consumer completion
        // must precede drawing into this image again.
        int32_t duplicateHandle() const;
    private:
        friend class ScarletSkiaVulkan;
        struct State;
        explicit Surface(std::unique_ptr<State>&&);
        std::unique_ptr<State> m_state;
    };
    // Owner-polled completion; only Complete permits external publication.
    enum class SubmissionStatus : uint8_t { Pending, Complete, Failed };
    class Submission final {
    public:
        ~Submission();
        Submission(const Submission&) = delete;
        Submission& operator=(const Submission&) = delete;
    private:
        friend class ScarletSkiaVulkan;
        struct State;
        explicit Submission(std::unique_ptr<State>&&);
        std::unique_ptr<State> m_state;
    };
    static std::shared_ptr<ScarletSkiaVulkan> create();
    ~ScarletSkiaVulkan();
    ScarletSkiaVulkan(const ScarletSkiaVulkan&) = delete;
    ScarletSkiaVulkan& operator=(const ScarletSkiaVulkan&) = delete;
    GrDirectContext* grContext() const;
    std::unique_ptr<Surface> createSurface(uint32_t width, uint32_t height, const SkSurfaceProps* = nullptr);
    // Wait for actual GPU completion before publishing a shared image.
    bool submit(Surface&);
    std::unique_ptr<Submission> submitAsync(Surface&);
    SubmissionStatus submissionStatus(Submission&);
    uint64_t allocatedBytes() const;
private:
    struct State;
    explicit ScarletSkiaVulkan(std::unique_ptr<State>&&);
    std::unique_ptr<State> m_state;
};
}
