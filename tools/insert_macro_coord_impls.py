from pathlib import Path
p = Path(r'C:\MirageWork\MirageVulkan\src\macro_api_server.cpp')
t = p.read_text(encoding='utf-8', errors='replace')
if 'std::string MacroApiServer::handle_normalize_coords' not in t:
    anchor = 'std::string MacroApiServer::handle_video_route(const std::string& device_id, const std::string& route, const std::string& host, int port) {'
    impl = '''std::string MacroApiServer::handle_normalize_coords(const std::string& device_id, int x, int y, int basis_w, int basis_h) {
    json r;
    if (basis_w <= 0 || basis_h <= 0) {
        r["status"] = "error";
        r["message"] = "invalid basis_w/basis_h";
        return r.dump();
    }
    double x_norm = static_cast<double>(x) / static_cast<double>(basis_w);
    double y_norm = static_cast<double>(y) / static_cast<double>(basis_h);
    if (x_norm < 0.0) x_norm = 0.0; if (x_norm > 1.0) x_norm = 1.0;
    if (y_norm < 0.0) y_norm = 0.0; if (y_norm > 1.0) y_norm = 1.0;

    int native_w = 0, native_h = 0;
    if (auto* mgr = ctx().adb_manager.get()) {
        auto devices = mgr->getUniqueDevices();
        for (const auto& ud : devices) {
            if (ud.hardware_id == device_id || ud.preferred_adb_id == device_id || ud.hardware_id == resolve_hw_id(device_id)) {
                native_w = ud.screen_width;
                native_h = ud.screen_height;
                break;
            }
        }
    }

    r["status"] = "ok";
    r["coord_basis"] = "native_normalized";
    r["x_norm"] = x_norm;
    r["y_norm"] = y_norm;
    r["source_basis_w"] = basis_w;
    r["source_basis_h"] = basis_h;
    r["native_w"] = native_w;
    r["native_h"] = native_h;
    return r.dump();
}

std::string MacroApiServer::handle_resolve_coords(const std::string& device_id, double x_norm, double y_norm, const std::string& prefer_space) {
    json r;
    if (x_norm < 0.0) x_norm = 0.0; if (x_norm > 1.0) x_norm = 1.0;
    if (y_norm < 0.0) y_norm = 0.0; if (y_norm > 1.0) y_norm = 1.0;

    std::string hw_id = resolve_hw_id(strip_route_prefix(device_id));
    int preview_w = 0, preview_h = 0;
    {
        std::lock_guard<std::mutex> lk(jpeg_cache_mutex_);
        auto it = jpeg_cache_.find(hw_id);
        if (it != jpeg_cache_.end()) {
            preview_w = it->second.width;
            preview_h = it->second.height;
        }
    }

    int native_w = 0, native_h = 0;
    if (auto* mgr = ctx().adb_manager.get()) {
        auto devices = mgr->getUniqueDevices();
        for (const auto& ud : devices) {
            if (ud.hardware_id == hw_id || ud.preferred_adb_id == strip_route_prefix(device_id)) {
                native_w = ud.screen_width;
                native_h = ud.screen_height;
                break;
            }
        }
    }

    int basis_w = native_w, basis_h = native_h;
    std::string coord_space = "native";
    if (prefer_space == "preview" && preview_w > 0 && preview_h > 0) {
        basis_w = preview_w;
        basis_h = preview_h;
        coord_space = "preview";
    } else if (basis_w <= 0 || basis_h <= 0) {
        if (preview_w > 0 && preview_h > 0) {
            basis_w = preview_w;
            basis_h = preview_h;
            coord_space = "preview";
        }
    }

    if (basis_w <= 0 || basis_h <= 0) {
        r["status"] = "error";
        r["message"] = "no basis size available";
        return r.dump();
    }

    int x = static_cast<int>(x_norm * static_cast<double>(basis_w) + 0.5);
    int y = static_cast<int>(y_norm * static_cast<double>(basis_h) + 0.5);

    r["status"] = "ok";
    r["coord_space"] = coord_space;
    r["x"] = x;
    r["y"] = y;
    r["basis_w"] = basis_w;
    r["basis_h"] = basis_h;
    r["preview_w"] = preview_w;
    r["preview_h"] = preview_h;
    r["native_w"] = native_w;
    r["native_h"] = native_h;
    r["coord_basis"] = "resolved_from_native_normalized";
    return r.dump();
}

'''
    if anchor not in t:
        raise SystemExit('anchor not found')
    t = t.replace(anchor, impl + anchor, 1)
    p.write_text(t, encoding='utf-8')
print('done')
