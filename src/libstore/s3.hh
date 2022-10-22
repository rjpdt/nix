#pragma once

#if ENABLE_S3

#include "ref.hh"

#include <optional>
#include <string>

namespace Aws { namespace Client { class ClientConfiguration; } }
namespace Aws { namespace S3 { class S3Client; } }
namespace Aws { namespace Utils { namespace Threading { class Executor; } } }

namespace nix {

struct Sink;

struct S3Helper
{
    ref<Aws::Client::ClientConfiguration> config;
    ref<Aws::S3::S3Client> client;
    std::shared_ptr<Aws::Utils::Threading::Executor> executor;

    S3Helper(const std::string & profile, const std::string & region, const std::string & scheme, const std::string & endpoint);

    ref<Aws::Client::ClientConfiguration> makeConfig(const std::string & region, const std::string & scheme, const std::string & endpoint);

    struct FileTransferResult
    {
        std::optional<std::string> data;
        std::optional<std::size_t> data_size;
        unsigned int durationMs;
    };

    FileTransferResult getObject(
        const std::string & bucketName, const std::string & key);
    FileTransferResult getObject(
        const std::string & bucketName, const std::string & key, Sink& sink);

    std::size_t getObjectSize(const std::string& bucketName, const std::string& key);
};

}

#endif
