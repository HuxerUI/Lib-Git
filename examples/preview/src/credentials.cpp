#include "credentials.h"

#if defined(__APPLE__)
#include <huxerui/app.h>
#include <Security/Security.h>
#include <TargetConditionals.h>

#include <memory>
#include <type_traits>
#endif

namespace git_preview {
namespace git = huxerui::git;

static git::GitError Unsupported() {
  return {git::ErrorCode::Unsupported, "Saved credentials require an HTTPS remote and an available system keychain"};
}

#if defined(__APPLE__)
namespace {

std::optional<std::string> HttpsEndpoint(std::string_view endpoint) {
  if (!endpoint.starts_with("https://") || endpoint.find_first_of("?#\r\n") != std::string_view::npos ||
      endpoint.find('\0') != std::string_view::npos) {
    return {};
  }
  endpoint.remove_prefix(8);
  const auto slash = endpoint.find('/');
  std::string authority(endpoint.substr(0, slash));
  if (authority.empty() || authority.find('@') != std::string::npos) return {};
  for (auto& character : authority) {
    if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
  }
  if (authority.ends_with(":443")) authority.resize(authority.size() - 4);
  if (authority.empty()) return {};
  const auto path = slash == std::string_view::npos ? std::string_view{"/"} : endpoint.substr(slash);
  return "https://" + authority + std::string(path);
}

constexpr std::string_view kAccountService = "org.huxerui.lib.git.preview.https-account/";
constexpr std::string_view kBindingService = "org.huxerui.lib.git.preview.remote-binding/";

struct ReleaseCF {
  void operator()(CFTypeRef value) const noexcept { CFRelease(value); }
};

template <class Ref>
using CFHandle = std::unique_ptr<std::remove_pointer_t<Ref>, ReleaseCF>;

CFHandle<CFStringRef> String(std::string_view text) {
  return CFHandle<CFStringRef>(CFStringCreateWithBytes(kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(text.data()), static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false));
}

CFHandle<CFMutableDictionaryRef> Dictionary() {
  return CFHandle<CFMutableDictionaryRef>(CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
}

/// Builds a generic-password query using the dedicated Preview service prefix.
/// @param service Keychain service name, including the normalized HTTPS endpoint.
/// @param account Username or repository binding key. Omit to list accounts for the endpoint.
/// @return An owned query, or an empty handle if a key could not be encoded.
CFHandle<CFMutableDictionaryRef> Item(std::string_view service, std::optional<std::string_view> account) {
  auto query = Dictionary();
  auto service_key = String(service);
  if (!query || !service_key) return {};
  CFDictionarySetValue(query.get(), kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(query.get(), kSecAttrService, service_key.get());
  if (account) {
    auto account_key = String(*account);
    if (!account_key) return {};
    CFDictionarySetValue(query.get(), kSecAttrAccount, account_key.get());
  }
  return query;
}

std::string BindingKey(const CredentialTarget& target) {
  const auto path = target.repository.Path();
  return std::to_string(path.size()) + ":" + path + target.remote;
}

git::GitError StorageError(OSStatus code) {
  return {code == errSecInteractionNotAllowed || code == errSecAuthFailed
              ? git::ErrorCode::PermissionDenied : git::ErrorCode::Io,
          "Could not access the system keychain (status " + std::to_string(code) + ")"};
}

/// Rejects reads that would need a system prompt while the credential provider runs on a worker.
/// @param query Keychain read query.
void DisableAuthenticationPrompts(CFMutableDictionaryRef query) {
  // The Security C API can fail the entire query, including when a binding exists but is locked.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  CFDictionarySetValue(query, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);
#pragma clang diagnostic pop
}

git::GitResult<std::optional<std::string>> ReadSecret(std::string_view service, std::string_view account) {
  using Result = git::GitResult<std::optional<std::string>>;
  auto query = Item(service, account);
  if (!query) return Result::Failure(StorageError(errSecParam));
  CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
  DisableAuthenticationPrompts(query.get());
  CFTypeRef value = nullptr;
  const auto status = SecItemCopyMatching(query.get(), &value);
  const CFHandle<CFTypeRef> owned(value);
  if (status == errSecItemNotFound) return Result::Success(std::nullopt);
  if (status != errSecSuccess) return Result::Failure(StorageError(status));
  if (!value || CFGetTypeID(value) != CFDataGetTypeID()) return Result::Failure(StorageError(errSecDecode));
  const auto data = static_cast<CFDataRef>(value);
  const auto size = CFDataGetLength(data);
  return Result::Success(size ? std::string(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                                            static_cast<std::size_t>(size)) : std::string{});
}

git::GitResult<void> WriteSecret(std::string_view service, std::string_view account, std::string_view secret) {
  using Result = git::GitResult<void>;
  auto query = Item(service, account);
  auto attributes = Dictionary();
  const CFHandle<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(secret.data()),
                                             static_cast<CFIndex>(secret.size())));
  if (!query || !attributes || !data) return Result::Failure(StorageError(errSecParam));
  CFDictionarySetValue(attributes.get(), kSecValueData, data.get());
  auto status = SecItemUpdate(query.get(), attributes.get());
  if (status == errSecItemNotFound) {
    auto item = Item(service, account);
    if (!item) return Result::Failure(StorageError(errSecParam));
    CFDictionarySetValue(item.get(), kSecValueData, data.get());
#if TARGET_OS_IPHONE
    CFDictionarySetValue(item.get(), kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
#endif
    status = SecItemAdd(item.get(), nullptr);
    if (status == errSecDuplicateItem) status = SecItemUpdate(query.get(), attributes.get());
  }
  return status == errSecSuccess ? Result::Success() : Result::Failure(StorageError(status));
}

std::optional<std::string> UniqueUsername(std::string_view service) {
  auto query = Item(service, {});
  if (!query) return {};
  CFDictionarySetValue(query.get(), kSecReturnAttributes, kCFBooleanTrue);
  CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitAll);
  DisableAuthenticationPrompts(query.get());
  CFTypeRef value = nullptr;
  const auto status = SecItemCopyMatching(query.get(), &value);
  const CFHandle<CFTypeRef> owned(value);
  if (status != errSecSuccess || !value || CFGetTypeID(value) != CFArrayGetTypeID()) return {};
  const auto accounts = static_cast<CFArrayRef>(value);
  if (CFArrayGetCount(accounts) != 1) return {};
  const auto attributes = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(accounts, 0));
  const auto account = static_cast<CFStringRef>(CFDictionaryGetValue(attributes, kSecAttrAccount));
  if (!account || CFGetTypeID(account) != CFStringGetTypeID()) return {};
  const auto capacity = CFStringGetMaximumSizeForEncoding(CFStringGetLength(account), kCFStringEncodingUTF8);
  if (capacity < 0) return {};
  std::string username(static_cast<std::size_t>(capacity) + 1, '\0');
  if (!CFStringGetCString(account, username.data(), capacity + 1, kCFStringEncodingUTF8)) return {};
  username.resize(std::char_traits<char>::length(username.c_str()));
  return username;
}

} // namespace

void InstallGit(huxerui::ApplicationContext& context) {
  context.Provide(std::make_shared<git::GitService>(FindSavedCredential));
}

bool CanSaveCredential(std::string_view endpoint) {
  return HttpsEndpoint(endpoint).has_value();
}

std::optional<git::Credential> FindSavedCredential(const git::CredentialRequest& request) {
  const auto& challenge = request.challenge;
  const auto endpoint = HttpsEndpoint(challenge.endpoint);
  if (!endpoint || !(challenge.allowed & static_cast<unsigned>(git::CredentialKind::UserPassword))) return {};
  std::optional<std::string> username;
  if (request.repository && !request.remote.empty()) {
    const auto binding = ReadCredentialBinding({*request.repository, request.remote, *endpoint});
    if (!binding.Succeeded()) return {};
    username = binding.Value();
  }
  const auto service = std::string(kAccountService) + *endpoint;
  if (username && !challenge.username.empty() && *username != challenge.username) return {};
  if (!username) username = challenge.username.empty() ? UniqueUsername(service)
                                                       : std::optional(challenge.username);
  if (!username || username->empty()) return {};
  auto token = ReadSecret(service, *username);
  if (!token.Succeeded() || !token.Value() || token.Value()->empty()) return {};
  return git::Credential{git::CredentialKind::UserPassword, std::move(*username), std::move(*std::move(token).Value())};
}

git::GitResult<std::optional<std::string>> ReadCredentialBinding(const CredentialTarget& target) {
  if (const auto endpoint = HttpsEndpoint(target.endpoint)) {
    return ReadSecret(std::string(kBindingService) + *endpoint, BindingKey(target));
  }
  return git::GitResult<std::optional<std::string>>::Failure(Unsupported());
}

git::GitResult<void> SaveCredential(std::string_view address, std::string_view username,
                                   std::string_view token, const std::optional<CredentialTarget>& target) {
  using Result = git::GitResult<void>;
  const auto endpoint = HttpsEndpoint(address);
  if (!endpoint) return Result::Failure(Unsupported());
  if (username.empty() || username.find('\0') != std::string_view::npos ||
      token.find('\0') != std::string_view::npos ||
      (target && (target->remote.empty() || HttpsEndpoint(target->endpoint) != endpoint))) {
    return Result::Failure({git::ErrorCode::InvalidArgument, "Supply an account and a matching HTTPS remote"});
  }
  const auto service = std::string(kAccountService) + *endpoint;
  if (token.empty()) {
    const auto saved = ReadSecret(service, username);
    if (!saved.Succeeded()) return Result::Failure(saved.Error());
    if (!saved.Value()) return Result::Failure({git::ErrorCode::NotFound, "Enter a token for this account"});
  } else {
    const auto saved = WriteSecret(service, username, token);
    if (!saved.Succeeded()) return saved;
  }
  if (target) return WriteSecret(std::string(kBindingService) + *endpoint, BindingKey(*target), username);
  return Result::Success();
}

git::GitResult<void> ClearCredentialBinding(const CredentialTarget& target) {
  using Result = git::GitResult<void>;
  if (const auto endpoint = HttpsEndpoint(target.endpoint)) {
    const auto query = Item(std::string(kBindingService) + *endpoint, BindingKey(target));
    if (!query) return Result::Failure(StorageError(errSecParam));
    const auto status = SecItemDelete(query.get());
    if (status == errSecSuccess || status == errSecItemNotFound) return Result::Success();
    return Result::Failure(StorageError(status));
  }
  return Result::Failure(Unsupported());
}

#else

void InstallGit(huxerui::ApplicationContext& context) {
  git::Install(context);
}

bool CanSaveCredential(std::string_view) {
  return false;
}

std::optional<git::Credential> FindSavedCredential(const git::CredentialRequest&) {
  return {};
}

git::GitResult<std::optional<std::string>> ReadCredentialBinding(const CredentialTarget&) {
  return git::GitResult<std::optional<std::string>>::Failure(Unsupported());
}

git::GitResult<void> SaveCredential(std::string_view, std::string_view, std::string_view,
                                   const std::optional<CredentialTarget>&) {
  return git::GitResult<void>::Failure(Unsupported());
}

git::GitResult<void> ClearCredentialBinding(const CredentialTarget&) {
  return git::GitResult<void>::Failure(Unsupported());
}

#endif

} // namespace git_preview
