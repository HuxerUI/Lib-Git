#pragma once

#include <huxerui/git.h>

namespace git_preview {

/// Registers the Git service with saved credentials on platforms that provide the Keychain integration.
/// @param context Application context that owns the service.
void InstallGit(huxerui::ApplicationContext& context);

/// Selects one account binding for an exact remote address.
struct CredentialTarget {
  /// Canonical Git directory of the local worktree.
  huxerui::File repository;
  /// Remote name in the local repository.
  std::string remote;
  /// HTTPS URL without embedded credentials.
  std::string endpoint;
};

/// @return Whether this build can save credentials for the given endpoint.
bool CanSaveCredential(std::string_view endpoint);

/// Looks up a saved account for a transport challenge on a Git worker.
/// @param request Repository, remote, and transport authentication context.
/// @return A matching credential or an empty value to request manual entry.
std::optional<huxerui::git::Credential> FindSavedCredential(const huxerui::git::CredentialRequest& request);

/// Reads the account bound to a repository and remote. Call on a worker.
/// @param target Repository and remote address to look up.
/// @return The bound username, no binding, or a storage error.
huxerui::git::GitResult<std::optional<std::string>> ReadCredentialBinding(const CredentialTarget& target);

/// Saves a token and optionally binds its account to a repository. Call on a worker.
/// @param endpoint HTTPS remote address for which the token is valid.
/// @param username Account that owns the token.
/// @param token New token, or an empty string to reuse the account's saved token.
/// @param target Repository and remote to bind, if supplied.
/// @return A successful result when the token and any requested binding were stored.
huxerui::git::GitResult<void> SaveCredential(std::string_view endpoint, std::string_view username,
                                            std::string_view token, const std::optional<CredentialTarget>& target);

/// Removes a repository's account binding. The saved account remains available to other repositories.
/// @param target Repository and remote address whose explicit binding is removed.
/// @return A successful result if the binding was removed or was already absent.
huxerui::git::GitResult<void> ClearCredentialBinding(const CredentialTarget& target);

} // namespace git_preview
