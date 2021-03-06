#pragma once

#include <string>
#include <unordered_map>
#include "core/reindexer.h"
#include "estl/shared_mutex.h"

/// @namespace reindexer_server
/// The namespace for reindexer server implementation
namespace reindexer_server {

using namespace reindexer;
using std::string;
using std::shared_ptr;
using std::unordered_map;

/// Possible user roles
enum UserRole {
	kUnauthorized,   /// User is not authorized
	kRoleNone,		 /// User is authenticaTed, but has no any righs
	kRoleDataRead,   /// User can read data from database
	kRoleDataWrite,  /// User can write data to database
	kRoleDBAdmin,	/// User can manage database: kRoleDataWrite + create & delete namespaces, modify indexes
	kRoleOwner,		 /// User can all privilegies on database: kRoleDBAdmin + create & drop database
};

const char *UserRoleName(UserRole role);

/// Record about user credentials
struct UserRecord {
	string login;							/// User's login
	string hash;							/// User's password or hash
	unordered_map<string, UserRole> roles;  /// map of user's roles on databases
};

class DBManager;

/// Context of user authentification
class AuthContext {
	friend DBManager;

public:
	/// Constuct empty context
	AuthContext() = default;
	/// Construct context with user credentials
	/// @param login - User's login
	/// @param password - User's password
	AuthContext(const string &login, const string &password) : login_(login), password_(password) {}
	/// Check if reqired role meets role from context, and get pointer to Reindexer DB object
	/// @param role - Requested role one of UserRole enum
	/// @param ret - Pointer to returned database pointer
	/// @return Error - error object
	Error GetDB(UserRole role, shared_ptr<Reindexer> *ret) {
		if (role > role_)
			return Error(errForbidden, "Forbidden: need role %s of db '%s' user '%s' have role=%s", UserRoleName(role), dbName_.c_str(),
						 login_.c_str(), UserRoleName(role_));
		*ret = db_;
		return errOK;
	}
	/// Reset Reindexer DB object pointer in context
	void ResetDB() {
		db_ = nullptr;
		dbName_.clear();
	}
	/// Check does context have an valid Reindexer DB object pointer
	/// @return true if db is ok
	bool HaveDB() { return db_ != nullptr; }
	/// Get user login
	/// @return user login
	const string &Login() { return login_; }
	/// Get database name
	/// @return db name
	const string &DBName() { return dbName_; }

protected:
	string login_;
	string password_;
	UserRole role_ = kUnauthorized;
	string dbName_;
	shared_ptr<Reindexer> db_;
};

/// Database manager. Control's available databases, users and their roles
class DBManager {
public:
	/// Construct DBManager
	/// @param dbpath - path to database on file system
	/// @param noSecurity - if true, then disable all security validations and users authentication
	DBManager(const string &dbpath, bool noSecurity);
	/// Initialize database:
	/// Read all found databases to RAM
	/// Read user's database
	/// @return Error - error object
	Error Init();
	/// Authenticate user, and grant roles to database with specified dbName
	/// @param dbName - database name. Can be empty.
	/// @param auth - AuthContext with user credentials
	/// @return Error - error object
	Error Login(const string &dbName, AuthContext &auth);
	/// Open database and authentificate user
	/// @param dbName - database name, Can't be empty
	/// @param auth - AuthContext filled with user credentials or already authorized AuthContext
	/// @param canCreate - true: Create database, if not exists; false: return error, if database not exists
	/// @return Error - error object
	Error OpenDatabase(const string &dbName, AuthContext &auth, bool canCreate);
	/// Drop database from disk storage and memory. Reindexer DB object will be destroyed
	/// @param auth - Authorized AuthContext, with valid Reindexer DB object and reasonale role
	/// @return Error - error object
	Error DropDatabase(AuthContext &auth);
	/// Check if security disabled
	/// @return bool - true: security checks are disabled; false: security checks are enabled
	bool IsNoSecurity() { return noSecurity_; }
	/// Enum list of available databases
	/// @return names of available databases
	vector<string> EnumDatabases();

private:
	Error readUsers();
	Error loadOrCreateDatabase(const string &name);

	unordered_map<string, shared_ptr<Reindexer>> dbs_;
	unordered_map<string, UserRecord> users_;
	string dbpath_;
	shared_timed_mutex mtx_;
	bool noSecurity_;
};

}  // namespace reindexer_server
