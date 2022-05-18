// This file is licensed under the Elastic License 2.0. Copyright 2021-present, StarRocks Limited.

#include "connector/jdbc_connector.h"

#include <sstream>

#include "exec/exec_node.h"
#include "exec/vectorized/jdbc_scanner.h"
#include "exprs/expr.h"
#include "runtime/jdbc_driver_manager.h"
#include "storage/chunk_helper.h"

namespace starrocks {
namespace connector {
using namespace vectorized;

// ================================

DataSourceProviderPtr JDBCConnector::create_data_source_provider(vectorized::ConnectorScanNode* scan_node,
                                                                 const TPlanNode& plan_node) const {
    return std::make_unique<JDBCDataSourceProvider>(scan_node, plan_node);
}

// ================================

JDBCDataSourceProvider::JDBCDataSourceProvider(vectorized::ConnectorScanNode* scan_node, const TPlanNode& plan_node)
        : _scan_node(scan_node), _jdbc_scan_node(plan_node.jdbc_scan_node) {}

DataSourcePtr JDBCDataSourceProvider::create_data_source(const TScanRange& scan_range) {
    return std::make_unique<JDBCDataSource>(this, scan_range);
}

// ================================

static std::string get_jdbc_sql(const std::string& table, const std::vector<std::string>& columns,
                                const std::vector<std::string>& filters, int64_t limit) {
    std::ostringstream oss;
    oss << "SELECT";
    for (size_t i = 0; i < columns.size(); i++) {
        oss << (i == 0 ? "" : ",") << " " << columns[i];
    }
    oss << " FROM " << table;
    if (!filters.empty()) {
        oss << " WHERE ";
        for (size_t i = 0; i < filters.size(); i++) {
            oss << (i == 0 ? "" : " AND") << "(" << filters[i] << ")";
        }
    }
    if (limit != -1) {
        oss << " LIMIT " << limit;
    }
    return oss.str();
}

JDBCDataSource::JDBCDataSource(const JDBCDataSourceProvider* provider, const TScanRange& scan_range)
        : _provider(provider) {}

Status JDBCDataSource::open(RuntimeState* state) {
    const TJDBCScanNode& jdbc_scan_node = _provider->_jdbc_scan_node;
    _runtime_state = state;
    _tuple_desc = state->desc_tbl().get_tuple_descriptor(jdbc_scan_node.tuple_id);
    RETURN_IF_ERROR(_create_scanner(state));
    return Status::OK();
}

void JDBCDataSource::close(RuntimeState* state) {
    if (_scanner != nullptr) {
        _scanner->reset_jni_env();
        _scanner->close(state);
    }
}

Status JDBCDataSource::get_next(RuntimeState* state, vectorized::ChunkPtr* chunk) {
    RETURN_IF_ERROR(_scanner->reset_jni_env());
    bool eos = false;
    _init_chunk(chunk, 0);
    do {
        RETURN_IF_ERROR(_scanner->get_next(state, chunk, &eos));
    } while (!eos && (*chunk)->num_rows() == 0);
    if (eos) {
        return Status::EndOfFile("");
    }
    _rows_read += (*chunk)->num_rows();
    return Status::OK();
}

int64_t JDBCDataSource::raw_rows_read() const {
    return _rows_read;
}
int64_t JDBCDataSource::num_rows_read() const {
    return _rows_read;
}

Status JDBCDataSource::_create_scanner(RuntimeState* state) {
    const TJDBCScanNode& jdbc_scan_node = _provider->_jdbc_scan_node;
    const auto* jdbc_table = down_cast<const JDBCTableDescriptor*>(_tuple_desc->table_desc());

    Status status;
    std::string driver_name = jdbc_table->jdbc_driver_name();
    std::string driver_url = jdbc_table->jdbc_driver_url();
    std::string driver_checksum = jdbc_table->jdbc_driver_checksum();
    std::string driver_class = jdbc_table->jdbc_driver_class();
    std::string driver_location;

    status = JDBCDriverManager::getInstance()->get_driver_location(driver_name, driver_url, driver_checksum,
                                                                   &driver_location);
    if (!status.ok()) {
        LOG(ERROR) << fmt::format("Get JDBC Driver[{}] error, error is {}", driver_name, status.to_string());
        return status;
    }

    vectorized::JDBCScanContext scan_ctx;
    scan_ctx.driver_path = driver_location;
    scan_ctx.driver_class_name = driver_class;
    scan_ctx.jdbc_url = jdbc_table->jdbc_url();
    scan_ctx.user = jdbc_table->jdbc_user();
    scan_ctx.passwd = jdbc_table->jdbc_passwd();
    scan_ctx.sql = get_jdbc_sql(jdbc_table->jdbc_table(), jdbc_scan_node.columns, jdbc_scan_node.filters, _read_limit);
    _scanner = _pool->add(new vectorized::JDBCScanner(scan_ctx, _tuple_desc, _runtime_profile));

    RETURN_IF_ERROR(_scanner->open(state));
    return Status::OK();
}

} // namespace connector
} // namespace starrocks
