void WkEsp32s3Dev::InitializeInfraredMcp() {
    auto mcp_server = McpServer::GetInstance();
    if (mcp_server == nullptr) {
        return;
    }

    PropertyList properties;
    properties.push_back({"device", "string", "Tên thiết bị hồng ngoại cần phát tín hiệu", true});

    mcp_server->AddTool(
        "self.ir.send", 
        "Phát tín hiệu hồng ngoại đã học", 
        properties, 
        [this](const PropertyList& args) -> ReturnValue {
            auto device_it = args.find("device");
            if (device_it != args.end()) {
                std::string device_name = std::get<std::string>(device_it->second);
                bool success = playIRCodeFromNVS(device_name);
                return success ? std::string("{\"status\": \"success\"}") : std::string("{\"status\": \"not_found\"}");
            }
            return std::string("{\"status\": \"error\", \"message\": \"Missing device parameter\"}");
        }
    );
}
