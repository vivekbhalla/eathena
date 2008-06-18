// Copyright (c) Athena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "../common/malloc.h"
#include "../common/mmo.h"
#include "../common/showmsg.h"
#include "../common/sql.h"
#include "../common/strlib.h"
#include "../common/timer.h"
#include "account.h"
#include <stdlib.h>
#include <string.h>


/// internal structure
typedef struct AccountDB_SQL
{
	AccountDB vtable;    // public interface

	Sql* accounts;       // SQL accounts storage
	int keepalive_timer; // ping timer id

	char db_hostname[32];
	uint16 db_port;
	char db_username[32];
	char db_password[32];
	char db_database[32];
	char codepage[32];
	bool case_sensitive;
	char account_db[32];
	char accreg_db[32];

} AccountDB_SQL;

/// internal structure
typedef struct AccountDBIterator_SQL
{
	AccountDBIterator vtable;    // public interface

	AccountDB_SQL* db;
	int last_account_id;
} AccountDBIterator_SQL;

/// internal functions
static bool account_db_sql_init(AccountDB* self);
static void account_db_sql_destroy(AccountDB* self);
static bool account_db_sql_get_property(AccountDB* self, const char* key, char* buf, size_t buflen);
static bool account_db_sql_set_property(AccountDB* self, const char* option, const char* value);
static bool account_db_sql_create(AccountDB* self, struct mmo_account* acc);
static bool account_db_sql_remove(AccountDB* self, const int account_id);
static bool account_db_sql_save(AccountDB* self, const struct mmo_account* acc);
static bool account_db_sql_load_num(AccountDB* self, struct mmo_account* acc, const int account_id);
static bool account_db_sql_load_str(AccountDB* self, struct mmo_account* acc, const char* userid);
static AccountDBIterator* account_db_sql_iterator(AccountDB* self);
static void account_db_sql_iter_destroy(AccountDBIterator* self);
static bool account_db_sql_iter_next(AccountDBIterator* self, struct mmo_account* acc);

static bool mmo_auth_fromsql(AccountDB_SQL* db, struct mmo_account* acc, int account_id);
static bool mmo_auth_tosql(AccountDB_SQL* db, const struct mmo_account* acc, bool is_new);
static int account_db_ping_init(AccountDB_SQL* db);
static int account_db_ping(int tid, unsigned int tick, int id, int data);

/// public constructor
AccountDB* account_db_sql(void)
{
	AccountDB_SQL* db = (AccountDB_SQL*)aCalloc(1, sizeof(AccountDB_SQL));

	// set up the vtable
	db->vtable.init         = &account_db_sql_init;
	db->vtable.destroy      = &account_db_sql_destroy;
	db->vtable.get_property = &account_db_sql_get_property;
	db->vtable.set_property = &account_db_sql_set_property;
	db->vtable.save         = &account_db_sql_save;
	db->vtable.create       = &account_db_sql_create;
	db->vtable.remove       = &account_db_sql_remove;
	db->vtable.load_num     = &account_db_sql_load_num;
	db->vtable.load_str     = &account_db_sql_load_str;
	db->vtable.iterator     = &account_db_sql_iterator;

	// initialize to default values
	db->accounts = NULL;
	db->keepalive_timer = INVALID_TIMER;
	safestrncpy(db->db_hostname, "127.0.0.1", sizeof(db->db_hostname));
	db->db_port = 3306;
	safestrncpy(db->db_username, "ragnarok", sizeof(db->db_username));
	safestrncpy(db->db_password, "ragnarok", sizeof(db->db_password));
	safestrncpy(db->db_database, "ragnarok", sizeof(db->db_database));
	safestrncpy(db->codepage, "", sizeof(db->codepage));
	db->case_sensitive = false;
	safestrncpy(db->account_db, "login", sizeof(db->account_db));
	safestrncpy(db->accreg_db, "global_reg_value", sizeof(db->accreg_db));

	return &db->vtable;
}


/* ------------------------------------------------------------------------- */


/// establishes database connection
static bool account_db_sql_init(AccountDB* self)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;
	Sql* sql_handle;

	db->accounts = Sql_Malloc();
	sql_handle = db->accounts;

	if( SQL_ERROR == Sql_Connect(sql_handle, db->db_username, db->db_password, db->db_hostname, db->db_port, db->db_database) )
	{
		Sql_ShowDebug(sql_handle);
		Sql_Free(db->accounts);
		db->accounts = NULL;
		return false;
	}

	if( db->codepage[0] != '\0' && SQL_ERROR == Sql_SetEncoding(sql_handle, db->codepage) )
		Sql_ShowDebug(sql_handle);

	account_db_ping_init(db);

	return true;
}	

/// disconnects from database
static void account_db_sql_destroy(AccountDB* self)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;

	delete_timer(db->keepalive_timer, account_db_ping);
	db->keepalive_timer = INVALID_TIMER;

	Sql_Free(db->accounts);
	db->accounts = NULL;
}

/// Gets a property from this database.
static bool account_db_sql_get_property(AccountDB* self, const char* key, char* buf, size_t buflen)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;
	const char* signature = "account.sql.";

	if( strcmp(key, "engine.name") == 0 )
	{
		safesnprintf(buf, buflen, "sql");
		return true;
	}
	if( strcmp(key, "engine.version") == 0 )
	{
		safesnprintf(buf, buflen, "???");
		return true;
	}
	if( strcmp(key, "engine.comment") == 0 )
	{
		safesnprintf(buf, buflen, "SQL Account Database");
		return true;
	}

	if( strncmp(key, signature, strlen(signature)) != 0 )
		return false;

	key += strlen(signature);

	if( strcmpi(key, "db_hostname") == 0 )
		safesnprintf(buf, buflen, "%s", db->db_hostname);
	else
	if( strcmpi(key, "db_port") == 0 )
		safesnprintf(buf, buflen, "%d", db->db_port);
	else
	if( strcmpi(key, "db_username") == 0 )
		safesnprintf(buf, buflen, "%s", db->db_username);
	else
	if( strcmpi(key, "db_password") == 0 )
		safesnprintf(buf, buflen, "%s", db->db_password);
	else
	if( strcmpi(key, "db_database") == 0 )
		safesnprintf(buf, buflen, "%s", db->db_database);
	else
	if( strcmpi(key, "codepage") == 0 )
		safesnprintf(buf, buflen, "%s", db->codepage);
	else
	if( strcmpi(key, "case_sensitive") == 0 )
		safesnprintf(buf, buflen, "%d", (db->case_sensitive ? 1 : 0));
	else
	if( strcmpi(key, "account_db") == 0 )
		safesnprintf(buf, buflen, "%s", db->account_db);
	else
	if( strcmpi(key, "accreg_db") == 0 )
		safesnprintf(buf, buflen, "%s", db->accreg_db);
	else
		return false;// not found

	return true;
}

/// if the option is supported, adjusts the internal state
static bool account_db_sql_set_property(AccountDB* self, const char* key, const char* value)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;
	const char* signature = "account.sql.";

	if( strncmp(key, signature, strlen(signature)) != 0 )
		return false;

	key += strlen(signature);

	if( strcmpi(key, "db_hostname") == 0 )
		safestrncpy(db->db_hostname, value, sizeof(db->db_hostname));
	else
	if( strcmpi(key, "db_port") == 0 )
		db->db_port = (uint16)strtoul(value, NULL, 10);
	else
	if( strcmpi(key, "db_username") == 0 )
		safestrncpy(db->db_username, value, sizeof(db->db_username));
	else
	if( strcmpi(key, "db_password") == 0 )
		safestrncpy(db->db_password, value, sizeof(db->db_password));
	else
	if( strcmpi(key, "db_database") == 0 )
		safestrncpy(db->db_database, value, sizeof(db->db_database));
	else
	if( strcmpi(key, "codepage") == 0 )
		safestrncpy(db->codepage, value, sizeof(db->codepage));
	else
	if( strcmpi(key, "case_sensitive") == 0 )
		db->case_sensitive = config_switch(value);
	else
	if( strcmpi(key, "account_db") == 0 )
		safestrncpy(db->account_db, value, sizeof(db->account_db));
	else
	if( strcmpi(key, "accreg_db") == 0 )
		safestrncpy(db->accreg_db, value, sizeof(db->accreg_db));
	else // no match
		return false;

	return true;
}

/// create a new account entry
/// If acc->account_id is -1, the account id will be auto-generated,
/// and its value will be written to acc->account_id if everything succeeds.
static bool account_db_sql_create(AccountDB* self, struct mmo_account* acc)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;
	Sql* sql_handle = db->accounts;

	// decide on the account id to assign
	int account_id;
	if( acc->account_id != -1 )
	{// caller specifies it manually
		account_id = acc->account_id;
	}
	else
	{// ask the database
		char* data;
		size_t len;

		if( SQL_SUCCESS != Sql_Query(sql_handle, "SELECT MAX(`account_id`)+1 FROM `%s`", db->account_db) )
		{
			Sql_ShowDebug(sql_handle);
			return false;
		}
		if( SQL_SUCCESS != Sql_NextRow(sql_handle) )
		{
			Sql_ShowDebug(sql_handle);
			Sql_FreeResult(sql_handle);
			return false;
		}

		Sql_GetData(sql_handle, 0, &data, &len);
		account_id = ( data != NULL ) ? atoi(data) : 0;
		Sql_FreeResult(sql_handle);

		if( account_id < START_ACCOUNT_NUM )
			account_id = START_ACCOUNT_NUM;

	}

	// zero value is prohibited
	if( account_id == 0 )
		return false;

	// absolute maximum
	if( account_id > END_ACCOUNT_NUM )
		return false;

	// insert the data into the database
	acc->account_id = account_id;
	return mmo_auth_tosql(db, acc, true);
}

/// delete an existing account entry + its regs
static bool account_db_sql_remove(AccountDB* self, const int account_id)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;
	Sql* sql_handle = db->accounts;
	bool result = false;

	if( SQL_SUCCESS != Sql_QueryStr(sql_handle, "START TRANSACTION")
	||  SQL_SUCCESS != Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `account_id` = %d", db->account_db, account_id)
	||  SQL_SUCCESS != Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `account_id` = %d", db->accreg_db, account_id) )
		Sql_ShowDebug(sql_handle);
	else
		result = true;

	result = ( SQL_SUCCESS == Sql_QueryStr(sql_handle, (result == true) ? "COMMIT" : "ROLLBACK") );

	return result;
}

/// update an existing account with the provided new data (both account and regs)
static bool account_db_sql_save(AccountDB* self, const struct mmo_account* acc)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;
	return mmo_auth_tosql(db, acc, false);
}

/// retrieve data from db and store it in the provided data structure
static bool account_db_sql_load_num(AccountDB* self, struct mmo_account* acc, const int account_id)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;
	return mmo_auth_fromsql(db, acc, account_id);
}

/// retrieve data from db and store it in the provided data structure
static bool account_db_sql_load_str(AccountDB* self, struct mmo_account* acc, const char* userid)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;
	Sql* sql_handle = db->accounts;
	char esc_userid[2*NAME_LENGTH+1];
	int account_id;
	char* data;

	Sql_EscapeString(sql_handle, esc_userid, userid);

	// get the list of account IDs for this user ID
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id` FROM `%s` WHERE `userid`= %s '%s'",
		db->account_db, (db->case_sensitive ? "BINARY" : ""), esc_userid) )
	{
		Sql_ShowDebug(sql_handle);
		return false;
	}

	if( Sql_NumRows(sql_handle) > 1 )
	{// serious problem - duplicit account
		ShowError("account_db_sql_load_str: multiple accounts found when retrieving data for account '%s'!\n", userid);
		Sql_FreeResult(sql_handle);
		return false;
	}

	if( SQL_SUCCESS != Sql_NextRow(sql_handle) )
	{// no such entry
		Sql_FreeResult(sql_handle);
		return false;
	}

	Sql_GetData(sql_handle, 0, &data, NULL);
	account_id = atoi(data);

	return account_db_sql_load_num(self, acc, account_id);
}


/// Returns a new forward iterator.
static AccountDBIterator* account_db_sql_iterator(AccountDB* self)
{
	AccountDB_SQL* db = (AccountDB_SQL*)self;
	AccountDBIterator_SQL* iter = (AccountDBIterator_SQL*)aCalloc(1, sizeof(AccountDBIterator_SQL));

	// set up the vtable
	iter->vtable.destroy = &account_db_sql_iter_destroy;
	iter->vtable.next    = &account_db_sql_iter_next;

	// fill data
	iter->db = db;
	iter->last_account_id = -1;

	return &iter->vtable;
}


/// Destroys this iterator, releasing all allocated memory (including itself).
static void account_db_sql_iter_destroy(AccountDBIterator* self)
{
	AccountDBIterator_SQL* iter = (AccountDBIterator_SQL*)self;
	aFree(iter);
}


/// Fetches the next account in the database.
static bool account_db_sql_iter_next(AccountDBIterator* self, struct mmo_account* acc)
{
	AccountDBIterator_SQL* iter = (AccountDBIterator_SQL*)self;
	AccountDB_SQL* db = (AccountDB_SQL*)iter->db;
	Sql* sql_handle = db->accounts;
	int account_id;
	char* data;

	// get next account ID
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `account_id` FROM `%s` WHERE `account_id` > '%d' ORDER BY `account_id` ASC LIMIT 1",
		db->account_db, iter->last_account_id) )
	{
		Sql_ShowDebug(sql_handle);
		return false;
	}

	if( SQL_SUCCESS == Sql_NextRow(sql_handle) &&
		SQL_SUCCESS == Sql_GetData(sql_handle, 0, &data, NULL) &&
		data != NULL )
	{// get account data
		account_id = atoi(data);
		if( mmo_auth_fromsql(db, acc, account_id) )
		{
			iter->last_account_id = account_id;
			Sql_FreeResult(sql_handle);
			return true;
		}
	}
	Sql_FreeResult(sql_handle);
	return false;
}


static int account_db_ping_init(AccountDB_SQL* db)
{
	uint32 connection_timeout, connection_ping_interval;
	Sql* sql_handle = db->accounts;

	// set a default value first
	connection_timeout = 28800; // 8 hours

	// ask the mysql server for the timeout value
	if( SQL_SUCCESS == Sql_GetTimeout(sql_handle, &connection_timeout) && connection_timeout < 60 )
		connection_timeout = 60;

	// establish keepalive
	connection_ping_interval = connection_timeout - 30; // 30-second reserve
	add_timer_func_list(account_db_ping, "account_db_ping");
	db->keepalive_timer = add_timer_interval(gettick() + connection_ping_interval*1000, account_db_ping, 0, (int)sql_handle, connection_ping_interval*1000);

	return 0;
}

static int account_db_ping(int tid, unsigned int tick, int id, int data)
{
	Sql* sql_handle = (Sql*)data;

	Sql_Ping(sql_handle);
	return 0;
}

static bool mmo_auth_fromsql(AccountDB_SQL* db, struct mmo_account* acc, int account_id)
{
	Sql* sql_handle = db->accounts;
	char* data;
	int i = 0;

	// retrieve login entry for the specified account
	if( SQL_ERROR == Sql_Query(sql_handle,
	    "SELECT `account_id`,`userid`,`user_pass`,`sex`,`email`,`level`,`state`,`unban_time`,`expiration_time`,`logincount`,`lastlogin`,`last_ip` FROM `%s` WHERE `account_id` = %d",
		db->account_db, account_id )
	) {
		Sql_ShowDebug(sql_handle);
		return false;
	}

	if( SQL_SUCCESS != Sql_NextRow(sql_handle) )
	{// no such entry
		Sql_FreeResult(sql_handle);
		return false;
	}

	Sql_GetData(sql_handle,  0, &data, NULL); acc->account_id = atoi(data);
	Sql_GetData(sql_handle,  1, &data, NULL); safestrncpy(acc->userid, data, sizeof(acc->userid));
	Sql_GetData(sql_handle,  2, &data, NULL); safestrncpy(acc->pass, data, sizeof(acc->pass));
	Sql_GetData(sql_handle,  3, &data, NULL); acc->sex = data[0];
	Sql_GetData(sql_handle,  4, &data, NULL); safestrncpy(acc->email, data, sizeof(acc->email));
	Sql_GetData(sql_handle,  5, &data, NULL); acc->level = atoi(data);
	Sql_GetData(sql_handle,  6, &data, NULL); acc->state = strtoul(data, NULL, 10);
	Sql_GetData(sql_handle,  7, &data, NULL); acc->unban_time = atol(data);
	Sql_GetData(sql_handle,  8, &data, NULL); acc->expiration_time = atol(data);
	Sql_GetData(sql_handle,  9, &data, NULL); acc->logincount = strtoul(data, NULL, 10);
	Sql_GetData(sql_handle, 10, &data, NULL); safestrncpy(acc->lastlogin, data, sizeof(acc->lastlogin));
	Sql_GetData(sql_handle, 11, &data, NULL); safestrncpy(acc->last_ip, data, sizeof(acc->last_ip));

	Sql_FreeResult(sql_handle);


	// retrieve account regs for the specified user
	if( SQL_ERROR == Sql_Query(sql_handle, "SELECT `str`,`value` FROM `%s` WHERE `type`='1' AND `account_id`='%d'", db->accreg_db, acc->account_id) )
	{
		Sql_ShowDebug(sql_handle);
		return false;
	}

	acc->account_reg2_num = (int)Sql_NumRows(sql_handle);

	while( SQL_SUCCESS == Sql_NextRow(sql_handle) )
	{
		char* data;
		Sql_GetData(sql_handle, 0, &data, NULL); safestrncpy(acc->account_reg2[i].str, data, sizeof(acc->account_reg2[i].str));
		Sql_GetData(sql_handle, 1, &data, NULL); safestrncpy(acc->account_reg2[i].value, data, sizeof(acc->account_reg2[i].value));
		++i;
	}
	Sql_FreeResult(sql_handle);

	if( i != acc->account_reg2_num )
		return false;

	return true;
}

static bool mmo_auth_tosql(AccountDB_SQL* db, const struct mmo_account* acc, bool is_new)
{
	Sql* sql_handle = db->accounts;
	SqlStmt* stmt = SqlStmt_Malloc(sql_handle);
	bool result = false;
	int i;

	// try
	do
	{

	if( SQL_SUCCESS != Sql_QueryStr(sql_handle, "START TRANSACTION") )
	{
		Sql_ShowDebug(sql_handle);
		break;
	}

	if( is_new )
	{// insert into account table
		if( SQL_SUCCESS != SqlStmt_Prepare(stmt,
			"INSERT INTO `%s` (`account_id`, `userid`, `user_pass`, `sex`, `email`, `level`, `state`, `unban_time`, `expiration_time`, `logincount`, `lastlogin`, `last_ip`) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
			db->account_db)
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  0, SQLDT_INT,    (void*)&acc->account_id,      sizeof(acc->account_id))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  1, SQLDT_STRING, (void*)acc->userid,           strlen(acc->userid))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  2, SQLDT_STRING, (void*)acc->pass,             strlen(acc->pass))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  3, SQLDT_ENUM,   (void*)&acc->sex,             sizeof(acc->sex))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  4, SQLDT_STRING, (void*)&acc->email,           strlen(acc->email))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  5, SQLDT_INT,    (void*)&acc->level,           sizeof(acc->level))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  6, SQLDT_UINT,   (void*)&acc->state,           sizeof(acc->state))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  7, SQLDT_LONG,   (void*)&acc->unban_time,      sizeof(acc->unban_time))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  8, SQLDT_INT,    (void*)&acc->expiration_time, sizeof(acc->expiration_time))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  9, SQLDT_UINT,   (void*)&acc->logincount,      sizeof(acc->logincount))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 10, SQLDT_STRING, (void*)&acc->lastlogin,       strlen(acc->lastlogin))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 11, SQLDT_STRING, (void*)&acc->last_ip,         strlen(acc->last_ip))
		||  SQL_SUCCESS != SqlStmt_Execute(stmt)
		) {
			SqlStmt_ShowDebug(stmt);
			break;
		}
	}
	else
	{// update account table
		stmt = SqlStmt_Malloc(sql_handle);
		if( SQL_SUCCESS != SqlStmt_Prepare(stmt, "UPDATE `%s` SET `userid`=?,`user_pass`=?,`sex`=?,`email`=?,`level`=?,`state`=?,`unban_time`=?,`expiration_time`=?,`logincount`=?,`lastlogin`=?,`last_ip`=? WHERE `account_id` = '%d'", db->account_db, acc->account_id)
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  0, SQLDT_STRING, (void*)acc->userid,           strlen(acc->userid))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  1, SQLDT_STRING, (void*)acc->pass,             strlen(acc->pass))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  2, SQLDT_ENUM,   (void*)&acc->sex,             sizeof(acc->sex))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  3, SQLDT_STRING, (void*)acc->email,            strlen(acc->email))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  4, SQLDT_INT,    (void*)&acc->level,           sizeof(acc->level))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  5, SQLDT_UINT,   (void*)&acc->state,           sizeof(acc->state))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  6, SQLDT_LONG,   (void*)&acc->unban_time,      sizeof(acc->unban_time))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  7, SQLDT_LONG,   (void*)&acc->expiration_time, sizeof(acc->expiration_time))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  8, SQLDT_UINT,   (void*)&acc->logincount,      sizeof(acc->logincount))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt,  9, SQLDT_STRING, (void*)&acc->lastlogin,       strlen(acc->lastlogin))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 10, SQLDT_STRING, (void*)&acc->last_ip,         strlen(acc->last_ip))
		||  SQL_SUCCESS != SqlStmt_Execute(stmt)
		) {
			SqlStmt_ShowDebug(stmt);
			break;
		}
	}

	// remove old account regs
	if( SQL_SUCCESS != Sql_Query(sql_handle, "DELETE FROM `%s` WHERE `type`='1' AND `account_id`='%d'", db->accreg_db, acc->account_id) )
	{
		Sql_ShowDebug(sql_handle);
		break;
	}
	// insert new account regs
	if( SQL_SUCCESS != SqlStmt_Prepare(stmt, "INSERT INTO `%s` (`type`, `account_id`, `str`, `value`) VALUES ( 1 , '%d' , ? , ? );",  db->accreg_db, acc->account_id) )
	{
		SqlStmt_ShowDebug(stmt);
		break;
	}
	for( i = 0; i < acc->account_reg2_num; ++i )
	{
		if( SQL_SUCCESS != SqlStmt_BindParam(stmt, 0, SQLDT_STRING, (void*)acc->account_reg2[i].str, strlen(acc->account_reg2[i].str))
		||  SQL_SUCCESS != SqlStmt_BindParam(stmt, 1, SQLDT_STRING, (void*)acc->account_reg2[i].value, strlen(acc->account_reg2[i].value))
		||  SQL_SUCCESS != SqlStmt_Execute(stmt)
		) {
			SqlStmt_ShowDebug(stmt);
			break;
		}
	}
	if( i < acc->account_reg2_num )
	{
		result = false;
		break;
	}

	// if we got this far, everything was successful
	result = true;

	} while(0);
	// finally

	result = ( SQL_SUCCESS == Sql_QueryStr(sql_handle, (result == true) ? "COMMIT" : "ROLLBACK") );
	SqlStmt_Free(stmt);

	return result;
}