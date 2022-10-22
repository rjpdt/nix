#if ENABLE_S3

#include "s3.hh"
#include "s3-binary-cache-store.hh"
#include "nar-info.hh"
#include "nar-info-disk-cache.hh"
#include "globals.hh"
#include "compression.hh"
#include "filetransfer.hh"
#include <future>
#include <memory>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>

#include <aws/core/Aws.h>
#include <aws/core/VersionConfig.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/utils/logging/FormattedLogSystem.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/threading/Executor.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/transfer/TransferManager.h>

using namespace Aws::Transfer;

namespace nix {

struct S3Error : public Error
{
    Aws::S3::S3Errors err;

    template<typename... Args>
    S3Error(Aws::S3::S3Errors err, const Args & ... args)
        : Error(args...), err(err) { };
};

/* Helper: given an Outcome<R, E>, return R in case of success, or
   throw an exception in case of an error. */
template<typename R, typename E>
R && checkAws(const FormatOrString & fs, Aws::Utils::Outcome<R, E> && outcome)
{
    if (!outcome.IsSuccess())
        throw S3Error(
            outcome.GetError().GetErrorType(),
            fs.s + ": " + outcome.GetError().GetMessage());
    return outcome.GetResultWithOwnership();
}

class AwsLogger : public Aws::Utils::Logging::FormattedLogSystem
{
    using Aws::Utils::Logging::FormattedLogSystem::FormattedLogSystem;

    void ProcessFormattedStatement(Aws::String && statement) override
    {
        debug("AWS: %s", chomp(statement));
    }

#if !(AWS_VERSION_MAJOR <= 1 && AWS_VERSION_MINOR <= 7 && AWS_VERSION_PATCH <= 115)
    void Flush() override {}
#endif
};

static void initAWS()
{
    static std::once_flag flag;
    std::call_once(flag, []() {
        Aws::SDKOptions options;

        /* We install our own OpenSSL locking function (see
           shared.cc), so don't let aws-sdk-cpp override it. */
        options.cryptoOptions.initAndCleanupOpenSSL = false;

        if (verbosity >= lvlDebug) {
            options.loggingOptions.logLevel =
                verbosity == lvlDebug
                ? Aws::Utils::Logging::LogLevel::Debug
                : Aws::Utils::Logging::LogLevel::Trace;
            options.loggingOptions.logger_create_fn = [options]() {
                return std::make_shared<AwsLogger>(options.loggingOptions.logLevel);
            };
        }

        Aws::InitAPI(options);
    });
}

namespace {
struct Chunk : std::enable_shared_from_this<Chunk>
{
    void complete()
    {
        // assert
        promise.set_value(shared_from_this());
    }

    Aws::IOStream* stream = nullptr;
    std::shared_ptr<TransferHandle> handle;
    std::promise<std::shared_ptr<Chunk>> promise;
};
}

S3Helper::S3Helper(
    const std::string & profile,
    const std::string & region,
    const std::string & scheme,
    const std::string & endpoint)
    : config(makeConfig(region, scheme, endpoint))
    , client(make_ref<Aws::S3::S3Client>(
            profile == ""
            ? std::dynamic_pointer_cast<Aws::Auth::AWSCredentialsProvider>(
                std::make_shared<Aws::Auth::DefaultAWSCredentialsProviderChain>())
            : std::dynamic_pointer_cast<Aws::Auth::AWSCredentialsProvider>(
                std::make_shared<Aws::Auth::ProfileConfigFileAWSCredentialsProvider>(profile.c_str())),
            *config,
            // FIXME: https://github.com/aws/aws-sdk-cpp/issues/759
#if AWS_VERSION_MAJOR == 1 && AWS_VERSION_MINOR < 3
            false,
#else
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
#endif
            endpoint.empty()))
    , executor{std::make_shared<Aws::Utils::Threading::PooledThreadExecutor>(std::thread::hardware_concurrency())}
{
}

/* Log AWS retries. */
class RetryStrategy : public Aws::Client::DefaultRetryStrategy
{
    bool ShouldRetry(const Aws::Client::AWSError<Aws::Client::CoreErrors>& error, long attemptedRetries) const override
    {
        auto retry = Aws::Client::DefaultRetryStrategy::ShouldRetry(error, attemptedRetries);
        if (retry)
            printError("AWS error '%s' (%s), will retry in %d ms",
                error.GetExceptionName(),
                error.GetMessage(),
                CalculateDelayBeforeNextRetry(error, attemptedRetries));
        return retry;
    }
};

ref<Aws::Client::ClientConfiguration> S3Helper::makeConfig(
    const std::string & region,
    const std::string & scheme,
    const std::string & endpoint)
{
    initAWS();
    auto res = make_ref<Aws::Client::ClientConfiguration>();
    res->region = region;
    if (!scheme.empty()) {
        res->scheme = Aws::Http::SchemeMapper::FromString(scheme.c_str());
    }
    if (!endpoint.empty()) {
        res->endpointOverride = endpoint;
    }
    res->requestTimeoutMs = 600 * 1000;
    res->connectTimeoutMs = 5 * 1000;
    res->retryStrategy = std::make_shared<RetryStrategy>();
    res->caFile = settings.caFile;
    return res;
}

S3Helper::FileTransferResult S3Helper::getObject(
    const std::string & bucketName, const std::string & key)
{
    debug("fetching 's3://%s/%s'...", bucketName, key);

    auto request =
        Aws::S3::Model::GetObjectRequest()
        .WithBucket(bucketName)
        .WithKey(key);

    request.SetResponseStreamFactory([&]() {
        return Aws::New<std::stringstream>("STRINGSTREAM");
    });

    FileTransferResult res;

    auto now1 = std::chrono::steady_clock::now();

    try {

        auto result = checkAws(fmt("AWS error fetching '%s'", key),
            client->GetObject(request));

        res.data = decompress(result.GetContentEncoding(),
            dynamic_cast<std::stringstream &>(result.GetBody()).str());

    } catch (S3Error & e) {
        if ((e.err != Aws::S3::S3Errors::NO_SUCH_KEY) &&
            (e.err != Aws::S3::S3Errors::ACCESS_DENIED)) throw;
    }

    auto now2 = std::chrono::steady_clock::now();

    res.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();

    return res;
}

S3Helper::FileTransferResult S3Helper::getObject(
    const std::string & bucketName, const std::string & key, Sink& sink)
{
    debug("fetching 's3://%s/%s'...", bucketName, key);

    auto now1 = std::chrono::steady_clock::now();

    auto createDownloadStreamFn = [](Chunk& chunk) {
        return [&]()->Aws::IOStream*{
            chunk.stream = Aws::New<std::stringstream>("STRINGSTREAM");
            return chunk.stream;
        };
    };

    std::mutex m;
    std::map<const TransferHandle*, std::unique_ptr<Chunk>> transfers;
    std::deque<std::future<std::shared_ptr<Chunk>>> chunks;
    std::size_t next_chunk_offset = 0;
    constexpr std::size_t chunk_size = 32 * 1024 * 1024;

    // Limit the number of simultaneous transfers. Should be at least 2 so that
    // we carry on downloading whilst writing to the sink.
    constexpr std::size_t max_transfers = 3;

    // Limit the number of chunks in flight. This caps memory usage to chunk_size * max_chunks
    constexpr std::size_t max_chunks = 5;
    const std::size_t object_size = getObjectSize(bucketName, key);


    std::function<void()> start_downloads;

    TransferManagerConfiguration config(executor.get());
    config.downloadProgressCallback = [&](const TransferManager*, const std::shared_ptr<const TransferHandle>& handle)
    {
        std::lock_guard<std::mutex> g{m};
        // TODO: tolerate handle not in transfers map?
        switch(handle->GetStatus())
        {
            case Aws::Transfer::TransferStatus::NOT_STARTED:
            case Aws::Transfer::TransferStatus::IN_PROGRESS:
                break;
            case Aws::Transfer::TransferStatus::COMPLETED:
                transfers[handle.get()]->complete();
                transfers.erase(handle.get());
                break;
            default:
                try {
                    // TODO: better message
                    // TODO: retry logic?
                    throw std::runtime_error("Error from AWS");
                } catch(const std::runtime_error& e) {
                    transfers[handle.get()]->promise.set_exception(std::current_exception());
                    // TODO: explicitly cancel the transfer?
                    transfers.erase(handle.get());
                }
                break;
        }
        start_downloads();
    };

    auto xfer_mgr = Aws::Transfer::TransferManager::Create(Aws::Transfer::TransferManagerConfiguration(config));

    start_downloads = [&]{
        while (next_chunk_offset < object_size and transfers.size() < max_transfers and chunks.size( ) < max_chunks)
        {
            auto chunk = std::make_unique<Chunk>();
            const auto download_size = std::min(chunk_size, object_size - next_chunk_offset);
            auto handle = xfer_mgr->DownloadFile(bucketName, key, next_chunk_offset, chunk_size, createDownloadStreamFn(*chunk));
            next_chunk_offset += download_size;
            chunk->handle = std::move(handle);
            chunks.push_back(chunk->promise.get_future());
            transfers[chunk->handle.get()] = std::move(chunk);
        }
    };

    {
        std::lock_guard<std::mutex> g{m};
        start_downloads();
    }

    while (not chunks.empty())
    {
        std::shared_ptr<Chunk> chunk;
        {
            std::lock_guard<std::mutex> g{m};
            chunk = std::move(chunks.front().get());
        }
        sink(dynamic_cast<std::stringstream&>(*chunk->stream).view());
        {
            std::lock_guard<std::mutex> g{m};
            chunks.pop_front();
            start_downloads();
        }
    }

/*
    auto request =
        Aws::S3::Model::GetObjectRequest()
        .WithBucket(bucketName)
        .WithKey(key);

    request.SetResponseStreamFactory([&]() {
        return Aws::New<std::stringstream>("STRINGSTREAM");
    });

    FileTransferResult res;

    auto now1 = std::chrono::steady_clock::now();

    try {

        auto result = checkAws(fmt("AWS error fetching '%s'", key),
            client->GetObject(request));

        res.data = decompress(result.GetContentEncoding(),
            dynamic_cast<std::stringstream &>(result.GetBody()).str());

    } catch (S3Error & e) {
        if ((e.err != Aws::S3::S3Errors::NO_SUCH_KEY) &&
            (e.err != Aws::S3::S3Errors::ACCESS_DENIED)) throw;
    }
    */

    auto now2 = std::chrono::steady_clock::now();

    FileTransferResult res;

    res.data_size = object_size;
    res.durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1).count();

    return res;
}

std::size_t S3Helper::getObjectSize(const std::string& bucketName, const std::string& key)
{
    Aws::S3::Model::HeadObjectRequest headObjectRequest;
    headObjectRequest.WithBucket(bucketName).WithKey(key);
    const auto headOutcome = checkAws(fmt("AWS error checking object size '%s'", key), client->HeadObject(headObjectRequest));
    return static_cast<std::size_t>(headOutcome.GetContentLength());
}

S3BinaryCacheStore::S3BinaryCacheStore(const Params & params)
    : BinaryCacheStoreConfig(params)
    , BinaryCacheStore(params)
{ }

struct S3BinaryCacheStoreConfig : virtual BinaryCacheStoreConfig
{
    using BinaryCacheStoreConfig::BinaryCacheStoreConfig;
    const Setting<std::string> profile{(StoreConfig*) this, "", "profile", "The name of the AWS configuration profile to use."};
    const Setting<std::string> region{(StoreConfig*) this, Aws::Region::US_EAST_1, "region", {"aws-region"}};
    const Setting<std::string> scheme{(StoreConfig*) this, "", "scheme", "The scheme to use for S3 requests, https by default."};
    const Setting<std::string> endpoint{(StoreConfig*) this, "", "endpoint", "An optional override of the endpoint to use when talking to S3."};
    const Setting<std::string> narinfoCompression{(StoreConfig*) this, "", "narinfo-compression", "compression method for .narinfo files"};
    const Setting<std::string> lsCompression{(StoreConfig*) this, "", "ls-compression", "compression method for .ls files"};
    const Setting<std::string> logCompression{(StoreConfig*) this, "", "log-compression", "compression method for log/* files"};
    const Setting<bool> multipartUpload{
        (StoreConfig*) this, false, "multipart-upload", "whether to use multi-part uploads"};
    const Setting<uint64_t> bufferSize{
        (StoreConfig*) this, 5 * 1024 * 1024, "buffer-size", "size (in bytes) of each part in multi-part uploads"};

    const std::string name() override { return "S3 Binary Cache Store"; }
};

struct S3BinaryCacheStoreImpl : virtual S3BinaryCacheStoreConfig, public virtual S3BinaryCacheStore
{
    std::string bucketName;

    Stats stats;

    S3Helper s3Helper;

    S3BinaryCacheStoreImpl(
        const std::string & uriScheme,
        const std::string & bucketName,
        const Params & params)
        : StoreConfig(params)
        , BinaryCacheStoreConfig(params)
        , S3BinaryCacheStoreConfig(params)
        , Store(params)
        , BinaryCacheStore(params)
        , S3BinaryCacheStore(params)
        , bucketName(bucketName)
        , s3Helper(profile, region, scheme, endpoint)
    {
        diskCache = getNarInfoDiskCache();
    }

    std::string getUri() override
    {
        return "s3://" + bucketName;
    }

    void init() override
    {
        if (auto cacheInfo = diskCache->cacheExists(getUri())) {
            wantMassQuery.setDefault(cacheInfo->wantMassQuery);
            priority.setDefault(cacheInfo->priority);
        } else {
            BinaryCacheStore::init();
            diskCache->createCache(getUri(), storeDir, wantMassQuery, priority);
        }
    }

    const Stats & getS3Stats() override
    {
        return stats;
    }

    /* This is a specialisation of isValidPath() that optimistically
       fetches the .narinfo file, rather than first checking for its
       existence via a HEAD request. Since .narinfos are small, doing
       a GET is unlikely to be slower than HEAD. */
    bool isValidPathUncached(const StorePath & storePath) override
    {
        try {
            queryPathInfo(storePath);
            return true;
        } catch (InvalidPath & e) {
            return false;
        }
    }

    bool fileExists(const std::string & path) override
    {
        stats.head++;

        auto res = s3Helper.client->HeadObject(
            Aws::S3::Model::HeadObjectRequest()
            .WithBucket(bucketName)
            .WithKey(path));

        if (!res.IsSuccess()) {
            auto & error = res.GetError();
            if (error.GetErrorType() == Aws::S3::S3Errors::RESOURCE_NOT_FOUND
                || error.GetErrorType() == Aws::S3::S3Errors::NO_SUCH_KEY
                // If bucket listing is disabled, 404s turn into 403s
                || error.GetErrorType() == Aws::S3::S3Errors::ACCESS_DENIED)
                return false;
            throw Error("AWS error fetching '%s': %s", path, error.GetMessage());
        }

        return true;
    }

    std::shared_ptr<TransferManager> transferManager;
    std::once_flag transferManagerCreated;

    void uploadFile(const std::string & path,
        std::shared_ptr<std::basic_iostream<char>> istream,
        const std::string & mimeType,
        const std::string & contentEncoding)
    {
        istream->seekg(0, istream->end);
        auto size = istream->tellg();
        istream->seekg(0, istream->beg);

        auto maxThreads = std::thread::hardware_concurrency();

        static std::shared_ptr<Aws::Utils::Threading::PooledThreadExecutor>
            executor = std::make_shared<Aws::Utils::Threading::PooledThreadExecutor>(maxThreads);

        std::call_once(transferManagerCreated, [&]()
        {
            if (multipartUpload) {
                TransferManagerConfiguration transferConfig(executor.get());

                transferConfig.s3Client = s3Helper.client;
                transferConfig.bufferSize = bufferSize;

                transferConfig.uploadProgressCallback =
                    [](const TransferManager *transferManager,
                        const std::shared_ptr<const TransferHandle>
                        &transferHandle)
                    {
                        //FIXME: find a way to properly abort the multipart upload.
                        //checkInterrupt();
                        debug("upload progress ('%s'): '%d' of '%d' bytes",
                            transferHandle->GetKey(),
                            transferHandle->GetBytesTransferred(),
                            transferHandle->GetBytesTotalSize());
                    };

                transferManager = TransferManager::Create(transferConfig);
            }
        });

        auto now1 = std::chrono::steady_clock::now();

        if (transferManager) {

            if (contentEncoding != "")
                throw Error("setting a content encoding is not supported with S3 multi-part uploads");

            std::shared_ptr<TransferHandle> transferHandle =
                transferManager->UploadFile(
                    istream, bucketName, path, mimeType,
                    Aws::Map<Aws::String, Aws::String>(),
                    nullptr /*, contentEncoding */);

            transferHandle->WaitUntilFinished();

            if (transferHandle->GetStatus() == TransferStatus::FAILED)
                throw Error("AWS error: failed to upload 's3://%s/%s': %s",
                    bucketName, path, transferHandle->GetLastError().GetMessage());

            if (transferHandle->GetStatus() != TransferStatus::COMPLETED)
                throw Error("AWS error: transfer status of 's3://%s/%s' in unexpected state",
                    bucketName, path);

        } else {

            auto request =
                Aws::S3::Model::PutObjectRequest()
                .WithBucket(bucketName)
                .WithKey(path);

            request.SetContentType(mimeType);

            if (contentEncoding != "")
                request.SetContentEncoding(contentEncoding);

            request.SetBody(istream);

            auto result = checkAws(fmt("AWS error uploading '%s'", path),
                s3Helper.client->PutObject(request));
        }

        auto now2 = std::chrono::steady_clock::now();

        auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now1)
                .count();

        printInfo("uploaded 's3://%s/%s' (%d bytes) in %d ms",
            bucketName, path, size, duration);

        stats.putTimeMs += duration;
        stats.putBytes += std::max(size, (decltype(size)) 0);
        stats.put++;
    }

    void upsertFile(const std::string & path,
        std::shared_ptr<std::basic_iostream<char>> istream,
        const std::string & mimeType) override
    {
        auto compress = [&](std::string compression)
        {
            auto compressed = nix::compress(compression, StreamToSourceAdapter(istream).drain());
            return std::make_shared<std::stringstream>(std::move(compressed));
        };

        if (narinfoCompression != "" && hasSuffix(path, ".narinfo"))
            uploadFile(path, compress(narinfoCompression), mimeType, narinfoCompression);
        else if (lsCompression != "" && hasSuffix(path, ".ls"))
            uploadFile(path, compress(lsCompression), mimeType, lsCompression);
        else if (logCompression != "" && hasPrefix(path, "log/"))
            uploadFile(path, compress(logCompression), mimeType, logCompression);
        else
            uploadFile(path, istream, mimeType, "");
    }

    void getFile(const std::string & path, Sink & sink) override
    {
        stats.get++;

        // FIXME: stream output to sink.
        auto res = s3Helper.getObject(bucketName, path, sink);

        stats.getBytes += res.data ? *res.data_size : 0;
        stats.getTimeMs += res.durationMs;

        if (res.data) {
            printTalkative("downloaded 's3://%s/%s' (%d bytes) in %d ms",
                bucketName, path, *res.data_size, res.durationMs);

        } else
            throw NoSuchBinaryCacheFile("file '%s' does not exist in binary cache '%s'", path, getUri());
    }

    StorePathSet queryAllValidPaths() override
    {
        StorePathSet paths;
        std::string marker;

        do {
            debug(format("listing bucket 's3://%s' from key '%s'...") % bucketName % marker);

            auto res = checkAws(format("AWS error listing bucket '%s'") % bucketName,
                s3Helper.client->ListObjects(
                    Aws::S3::Model::ListObjectsRequest()
                    .WithBucket(bucketName)
                    .WithDelimiter("/")
                    .WithMarker(marker)));

            auto & contents = res.GetContents();

            debug(format("got %d keys, next marker '%s'")
                % contents.size() % res.GetNextMarker());

            for (auto object : contents) {
                auto & key = object.GetKey();
                if (key.size() != 40 || !hasSuffix(key, ".narinfo")) continue;
                paths.insert(parseStorePath(storeDir + "/" + key.substr(0, key.size() - 8) + "-" + MissingName));
            }

            marker = res.GetNextMarker();
        } while (!marker.empty());

        return paths;
    }

    static std::set<std::string> uriSchemes() { return {"s3"}; }

};

static RegisterStoreImplementation<S3BinaryCacheStoreImpl, S3BinaryCacheStoreConfig> regS3BinaryCacheStore;

}

#endif
