#include <memory>
#include <iostream>
#include <utility>

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/error/error.h"

#include "util.hpp"
#include "conn.hpp"
#include "serv.hpp"
#include "myapi.hpp"

using namespace rapidjson;
using namespace boost;

static const char* kTypeNames[] = { "Null", "False", "True", "Object", "Array", "String", "Number" };

std::string validate_request(char *data, Document *doc, std::vector<std::pair<std::string, Type>> requires){
	std::stringstream response;

	doc->Parse(data);
	if (doc->HasParseError() && requires.size() > 0){
		int offset = doc->GetErrorOffset();
		response << "Invalid JSON at character " << offset << ": '" << data[offset] << "'.";

		return simple_error_json(response.str());
	}

	for (std::vector<int>::size_type i = 0; i != requires.size(); i++){
		if (!doc->HasMember(requires[i].first.c_str())
		|| (*doc)[requires[i].first.c_str()].GetType() != requires[i].second){
			response << "'" << requires[i].first << "' requires a " << kTypeNames[requires[i].second] << ".";

			return simple_error_json(response.str());
		}
	}

	return std::string();
}

Connection::Connection(WebService* service, asio::io_service& io_service)
	: conn_socket(io_service),
	serv_reference(service) {
}

void Connection::start_receiving(){
	// Connection success.
	std::cout << "CONN: |" << this->conn_socket.remote_endpoint().address().to_string() << std::endl;

	conn_socket.async_read_some(asio::buffer(received_data, 8192), bind(&Connection::received, this, asio::placeholders::error, asio::placeholders::bytes_transferred));
}

void Connection::received(const system::error_code& ec, std::size_t length) {
	if (ec) {
		std::cout << "received async_read_some error (" << ec.value() << "): " << ec.message() << std::endl;
		return;
	}
	received_data[length] = '\0';

	char *pstart = strstr(received_data, " ") + 1;
	char *pend = strstr(pstart, " ");

	std::string path(pstart, pend - pstart);
	std::cout << "PATH: |" << path << "|" << std::endl;

	char *received_body = strstr(received_data, "\r\n\r\n") + 4;
	std::cout << "DATA: |" << received_body << "|" << std::endl;

	std::string delivery_json;

	if (serv_reference->routes.count(path)){
		auto f = serv_reference->routes[path];

		Document doc;

		auto result = validate_request(received_body, &doc, serv_reference->required_fields[path]);
		if (!result.empty()){
			delivery_json = result;
		}
		else{
			delivery_json = f(&doc);
		}
	}
	else{
		delivery_json = "{\"error\":\"No resource.\"}";
	}

	delivery_data = "HTTP/1.1 200 OK\n";
	delivery_data.append("Connection: close\n");
	delivery_data.append("Server: " + serv_reference->GetName() + "\n");
	delivery_data.append("Accept-Ranges: bytes\n");
	delivery_data.append("Content-Type: application/json\n");
	delivery_data.append("Content-Length: " + std::to_string(delivery_json.length()) + "\n");
	delivery_data.append("\n");
	delivery_data.append(delivery_json);
	
	std::cout << "DELI: |" << delivery_json << "|" << std::endl;

	conn_socket.async_write_some(asio::buffer(delivery_data, delivery_data.length()), bind(&Connection::delivered_done, this, asio::placeholders::error, asio::placeholders::bytes_transferred));
}

void Connection::delivered_done(const system::error_code& ec, std::size_t length) {
	if (ec) {
		std::cout << "delivered_done async_write_some error (" << ec.value() << "): " << ec.message() << std::endl;
	}

	//HTTP 1.1
	conn_socket.async_read_some(asio::buffer(received_data, 1024), bind(&Connection::received, this, asio::placeholders::error, asio::placeholders::bytes_transferred));

	//HTTP 1.0
	//conn_socket.shutdown(asio::ip::tcp::socket::shutdown_both);
	//conn_socket.close();
	//delete this;
}