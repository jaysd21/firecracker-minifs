#pragma once
#include "remote_disk/disk_config.h"
#include "remote_disk/storage_client.h"

#include <memory>
#include <string>

namespace remote_disk {

/* StorageClient backed by NebulaR's ddl::runtime::CoreClient. One instance
 * owns exactly one CoreClient; create and use it on one thread only. */
class MiniFsAdapter final : public StorageClient {
public:
    ~MiniFsAdapter() override;

    /* Starts the client and waits up to timeout_seconds for the connection.
     * Returns nullptr and fills error on failure. */
    static std::unique_ptr<MiniFsAdapter> connect(const MiniFsConfig& config,
                                                  std::string* error);

    bool ready() override;
    IoStatus submit_get(const GetRequest& request) override;
    size_t poll(Completion* out, size_t max) override;
    void stop() override;

private:
    struct Impl;
    MiniFsAdapter();
    std::unique_ptr<Impl> impl_;
};

} // namespace remote_disk
