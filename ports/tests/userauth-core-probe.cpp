// Exercise the real portable backend against a separately compiled fixture
// storage directory. Internal metadata seeding tests persistence only; it is
// never used for the real account or claimed as PIN enrollment/authentication.
#include "v4_0/iuser_auth_interface.h"
#include "idm_database.h"
#include "idm_common.h"
#include "file_operator.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <unistd.h>

using OHOS::HDI::UserAuth::V4_0::IUserAuthInterface;
using OHOS::HDI::UserAuth::V4_0::CredentialInfo;
using OHOS::HDI::UserAuth::V4_0::UserAuthTokenPlain;

int main(int argc, char **argv)
{
    assert(argc == 2 && geteuid() == 2000);
    const std::string mode = argv[1];
    void *lib = dlopen("/system/bin/aurora/userauth-fixture/libaurora_userauth_fixture.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::printf("FIXTURE_DLOPEN_ERROR %s\n", dlerror()); return 2; }
    auto factory = reinterpret_cast<IUserAuthInterface *(*)()>(dlsym(lib, "UserAuthInterfaceImplGetInstance"));
    auto fileOps = reinterpret_cast<FileOperator *(*)()>(dlsym(lib, "GetDefaultFileOperator"));
    auto add = reinterpret_cast<ResultCode (*)(int32_t, CredentialInfoHal *, int32_t)>(dlsym(lib, "AddCredentialInfo"));
    assert(factory && fileOps && add);
    auto *files = fileOps();
    constexpr const char *path = "/data/service/el1/public/userauth/userinfo";
    if (mode == "empty") {
        // Canonical version-0 empty fixture DB, explicitly provisioned once.
        const uint32_t empty[] = {0, 0};
        assert(files->writeFile(path, reinterpret_cast<const uint8_t *>(empty), sizeof(empty)) == RESULT_SUCCESS);
    } else if (mode == "corrupt-version" || mode == "corrupt-trailing") {
        const uint32_t bad[] = {mode == "corrupt-version" ? 1U : 0U, 0U, 0U};
        uint32_t length = mode == "corrupt-version" ? 8 : 12;
        assert(files->writeFile(path, reinterpret_cast<const uint8_t *>(bad), length) == RESULT_SUCCESS);
    } else if (mode == "missing") {
        assert(files->deleteFile(path) == RESULT_SUCCESS);
    } else assert(mode == "seed" || mode == "reload");
    auto *service = factory();
    assert(service);
    // The real protocol requires a 64-byte UDID. This test value belongs only
    // to the isolated fixture and is never supplied as a product identity.
    int32_t init = service->Init(std::string(64, 'f'));
    std::printf("USERAUTH_FIXTURE_INIT mode=%s result=%d\n", mode.c_str(), init);
    std::fflush(stdout);
    std::vector<CredentialInfo> infos;
    if (mode == "corrupt-version" || mode == "corrupt-trailing" || mode == "missing") {
        assert(init != 0 && service->GetCredential(4242, 1, infos) != 0 && infos.empty());
        std::printf("USERAUTH_BAD_STATE_REJECTED mode=%s init=%d\n", mode.c_str(), init);
        delete service;
        return 0;
    }
    assert(init == 0);
    assert(service->GetCredential(100, 1, infos) == 0 && infos.empty());
    if (mode == "seed") {
        CredentialInfoHal credential = {};
        credential.templateId = 0x42420001;
        credential.authType = PIN_AUTH;
        credential.executorSensorHint = 7;
        credential.executorMatcher = 8;
        credential.capabilityLevel = 1;
        assert(add(4242, &credential, MAIN_USER) == RESULT_SUCCESS);
    }
    infos.clear();
    assert(service->GetCredential(4242, 1, infos) == 0);
    if (mode == "empty") assert(infos.empty());
    else assert(infos.size() == 1 && infos[0].templateId == 0x42420001 && infos[0].credentialId != 0);
    UserAuthTokenPlain plain = {};
    std::vector<uint8_t> rootSecret;
    const std::vector<uint8_t> invalidToken = {1, 2, 3, 4};
    int32_t rejected = service->VerifyAuthToken(invalidToken, 1000, plain, rootSecret);
    assert(rejected != 0 && rootSecret.empty());
    std::printf("USERAUTH_REAL_STORAGE_QUERY_PASS mode=%s credentials=%zu bad_token_result=%d\n",
                mode.c_str(), infos.size(), rejected);
    delete service;
}
