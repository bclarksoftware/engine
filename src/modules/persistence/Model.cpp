/**
 * @file
 */

#include "Model.h"
#include "ConnectionPool.h"
#include "ScopedConnection.h"
#include "core/Log.h"
#include "core/String.h"
#include <algorithm>

namespace persistence {

Model::Model(const std::string& tableName) :
		_tableName(tableName) {
	_membersPointer = (uint8_t*)this;
}

Model::~Model() {
	_fields.clear();
}

bool Model::isPrimaryKey(const std::string& fieldname) const {
	auto i = std::find_if(_fields.begin(), _fields.end(),
			[&fieldname](const Field& f) {return f.name == fieldname;}
	);
	if (i == _fields.end()) {
		return false;
	}

	return (i->contraintMask & Model::PRIMARYKEY) != 0;
}

Model::PreparedStatement Model::prepare(const std::string& name, const std::string& statement) {
	return PreparedStatement(this, name, statement);
}

bool Model::checkLastResult(State& state, Connection* connection) const {
	state.affectedRows = 0;
	if (state.res == nullptr) {
		Log::debug("Empty result");
		return false;
	}

#ifdef PERSISTENCE_POSTGRES
	ExecStatusType lastState = PQresultStatus(state.res);

	switch (lastState) {
	case PGRES_NONFATAL_ERROR:
		state.lastErrorMsg = PQerrorMessage(connection->connection());
		Log::warn("non fatal error: %s", state.lastErrorMsg.c_str());
		break;
	case PGRES_BAD_RESPONSE:
	case PGRES_FATAL_ERROR:
		state.lastErrorMsg = PQerrorMessage(connection->connection());
		Log::error("fatal error: %s", state.lastErrorMsg.c_str());
		PQclear(state.res);
		state.res = nullptr;
		return false;
	case PGRES_EMPTY_QUERY:
	case PGRES_COMMAND_OK:
	case PGRES_TUPLES_OK:
		state.affectedRows = PQntuples(state.res);
		Log::debug("Affected rows %i", state.affectedRows);
		break;
	default:
		Log::error("not catched state: %s", PQresStatus(lastState));
		return false;
	}
#endif

	state.result = true;
	return true;
}

bool Model::exec(const char* query) {
	Log::debug("%s", query);
	ScopedConnection scoped(ConnectionPool::get().connection());
	if (scoped == false) {
		Log::error("Could not execute query '%s' - could not acquire connection", query);
		return false;
	}
	ConnectionType* conn = scoped.connection()->connection();
#ifdef PERSISTENCE_POSTGRES
	State s(PQexec(conn, query));
	checkLastResult(s, scoped);
	return fillModelValues(s);
#elif defined PERSISTENCE_SQLITE
	char *zErrMsg = nullptr;
	const int rc = sqlite3_exec(conn, query, nullptr, nullptr, &zErrMsg);
	if (rc != SQLITE_OK) {
		if (zErrMsg != nullptr) {
			Log::error("SQL error: %s", zErrMsg);
		}
		sqlite3_free(zErrMsg);
		return false;
	}

	return true;
#else
	return false;
#endif
}

Model::Field Model::getField(const std::string& name) const {
	for (const Field& field : _fields) {
		if (field.name == name) {
			return field;
		}
	}
	return Field();
}

bool Model::begin() {
	return exec("START TRANSACTION;");
}

bool Model::commit() {
	return exec("COMMIT;");
}

bool Model::rollback() {
	return exec("ROLLBACK;");
}

bool Model::fillModelValues(Model::State& state) {
	// TODO: 0 even in case a key was generated
	if (state.affectedRows > 1) {
		Log::debug("More than one row affected, can't fill model values");
		return state.result;
	} else if (state.affectedRows <= 0) {
		Log::trace("No rows affected, can't fill model values");
		return state.result;
	}

#ifdef PERSISTENCE_POSTGRES
	const int nFields = PQnfields(state.res);
#else
	//sqlite3_int64 sqlite3_last_insert_rowid(sqlite3*);
	const int nFields = 0;
#endif
	Log::info("Query has values for %i fields", nFields);
	for (int i = 0; i < nFields; ++i) {
#ifdef PERSISTENCE_POSTGRES
		const char* name = PQfname(state.res, i);
		const char* value = PQgetvalue(state.res, 0, i);
#else
		const char* name = "NOT_IMPLEMENTED";
		const char* value = "";
#endif
		if (value == nullptr) {
			value = "";
		}
		const Field& f = getField(name);
		if (f.name != name) {
			Log::error("Unknown field name for '%s'", name);
			state.result = false;
			return false;
		}
		Log::debug("Try to set '%s' to '%s'", name, value);
		switch (f.type) {
		case Model::STRING:
		case Model::PASSWORD:
			setValue(f, std::string(value));
			break;
		case Model::INT:
			setValue(f, core::string::toInt(value));
			break;
		case Model::LONG:
			setValue(f, core::string::toLong(value));
			break;
		case Model::TIMESTAMP: {
			setValue(f, Timestamp(core::string::toLong(value)));
			break;
		}
		}
	}
	return true;
}

Model::PreparedStatement::PreparedStatement(Model* model, const std::string& name, const std::string& statement) :
		_model(model), _name(name), _statement(statement) {
}

Model::State Model::PreparedStatement::exec() {
	Log::debug("prepared statement: '%s'", _statement.c_str());
	ScopedConnection scoped(ConnectionPool::get().connection());
	if (scoped == false) {
		Log::error("Could not prepare query '%s' - could not acquire connection", _statement.c_str());
		return State(nullptr);
	}

	ConnectionType* conn = scoped.connection()->connection();

#ifdef PERSISTENCE_POSTGRES
	if (!scoped.connection()->hasPreparedStatement(_name)) {
		State state(PQprepare(conn, _name.c_str(), _statement.c_str(), (int)_params.size(), nullptr));
		if (!_model->checkLastResult(state, scoped)) {
			return state;
		}
		scoped.connection()->registerPreparedStatement(_name);
	}
	const int size = _params.size();
	const char* paramValues[size];
	// TODO: handle the type
	for (int i = 0; i < size; ++i) {
		paramValues[i] = _params[i].first.c_str();
	}
	State prepState(PQexecPrepared(conn, _name.c_str(), size, paramValues, nullptr, nullptr, 0));
	if (!_model->checkLastResult(prepState, scoped)) {
		return prepState;
	}
	_model->fillModelValues(prepState);
	return prepState;
#elif defined PERSISTENCE_SQLITE
	ResultType *stmt;
	const char *pzTest;
	const int rcPrep = sqlite3_prepare_v2(conn, _statement.c_str(), _statement.size(), &stmt, &pzTest);
	if (rcPrep != SQLITE_OK) {
		Log::error("failed to prepare the statement: %s", sqlite3_errmsg(conn));
		return State(nullptr);
	}
	sqlite3_reset(stmt);

	const int size = _params.size();
	for (int i = 0; i < size; ++i) {
		const int retVal = sqlite3_bind_text(stmt, i, _params[i].c_str(), _params[i].size(), SQLITE_TRANSIENT);
		if (retVal != SQLITE_OK) {
			Log::error("Failed to bind: %s", sqlite3_errmsg(conn));
			return State(nullptr);
		}
	}

	char *zErrMsg = nullptr;
	const int rcExec = sqlite3_exec(conn, _statement.c_str(), nullptr, nullptr, &zErrMsg);
	if (rcExec != SQLITE_OK) {
		if (zErrMsg != nullptr) {
			Log::error("Failed to exec: %s", zErrMsg);
		}
		sqlite3_free(zErrMsg);
		return State(nullptr);
	}

	State state(stmt);
	_model->fillModelValues();
	return state;
#else
	return State(nullptr);
#endif
}

Model::State::State(ResultType* _res) :
		res(_res) {
}

Model::State::State(State&& other) :
		res(other.res), lastErrorMsg(other.lastErrorMsg), affectedRows(
				other.affectedRows), result(other.result) {
	other.res = nullptr;
}

Model::State::~State() {
	if (res != nullptr) {
#ifdef PERSISTENCE_POSTGRES
		PQclear(res);
#elif defined PERSISTENCE_SQLITE
		const int retVal = sqlite3_finalize(res);
		if (retVal != SQLITE_OK) {
			//const char *errMsg = sqlite3_errmsg(_pgConnection);
			Log::error("Could not finalize the statement");
		}
#endif
		res = nullptr;
	}
}

Model::ScopedTransaction::ScopedTransaction(Model* model, bool autocommit) :
		_autocommit(autocommit), _model(model) {
}

Model::ScopedTransaction::~ScopedTransaction() {
	if (_autocommit) {
		commit();
	} else {
		rollback();
	}
}

void Model::ScopedTransaction::commit() {
	if (_commited) {
		return;
	}
	_commited = true;
	_model->commit();
}

void Model::ScopedTransaction::rollback() {
	if (_commited) {
		return;
	}
	_commited = true;
	_model->rollback();
}

}
