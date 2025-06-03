#pragma once
#include <string>

void RunServer(const std::string &address);

std::string PingClient(const std::string &address);