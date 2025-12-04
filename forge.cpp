#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <lua.hpp>

namespace fs = std::filesystem;

struct Target {
    std::string name;
    std::vector<std::string> depends;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    int lua_func_ref = LUA_NOREF;
    int on_execute_ref = LUA_NOREF;
    int after_execute_ref = LUA_NOREF;
    bool phony = false;
};

class Forge {
private:
    std::map<std::string, Target> targets;
    std::set<std::string> executed;
    std::set<std::string> in_progress;
    std::mutex exec_mutex;
    std::condition_variable exec_cv;
    std::atomic<bool> has_error{false};
    bool dry_run = false;
    bool verbose = false;
    int max_jobs = 1;
    bool rebuild = false;
    lua_State* L = nullptr;

    fs::file_time_type get_newest_time(const std::vector<std::string>& files) {
        auto newest = fs::file_time_type::min();
        for (const auto& file : files) {
            if (fs::exists(file)) {
                auto ftime = fs::last_write_time(file);
                if (ftime > newest) {
                    newest = ftime;
                }
            }
        }
        return newest;
    }
    
    fs::file_time_type get_oldest_time(const std::vector<std::string>& files) {
        auto oldest = fs::file_time_type::max();
        bool found_any = false;
        for (const auto& file : files) {
            if (fs::exists(file)) {
                found_any = true;
                auto ftime = fs::last_write_time(file);
                if (ftime < oldest) {
                    oldest = ftime;
                }
            }
        }
        return found_any ? oldest : fs::file_time_type::min();
    }
    
    bool needs_rebuild(const Target& target) {
        if (rebuild) return true;
        if (target.phony) return true;
        if (target.outputs.empty()) return true;
        
        bool all_outputs_exist = true;
        for (const auto& output : target.outputs) {
            if (!fs::exists(output)) {
                all_outputs_exist = false;
                break;
            }
        }
        
        if (!all_outputs_exist) return true;
        if (target.inputs.empty()) return false;
        
        auto newest_input = get_newest_time(target.inputs);
        auto oldest_output = get_oldest_time(target.outputs);
        
        return newest_input > oldest_output;
    }
    
    bool can_execute(const std::string& name) {
        if (!targets.count(name)) return false;
        
        Target& target = targets[name];
        for (const auto& dep : target.depends) {
            if (!executed.count(dep) || in_progress.count(dep)) {
                return false;
            }
        }
        return true;
    }
    
    static int lua_target(lua_State* L) {
        Forge* forge = static_cast<Forge*>(lua_touserdata(L, lua_upvalueindex(1)));
        
        if (!lua_istable(L, 1)) {
            return luaL_error(L, "target expects a table");
        }
        
        Target target;
        
        lua_getfield(L, 1, "name");
        if (lua_isstring(L, -1)) {
            target.name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
        
        lua_getfield(L, 1, "depends");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (lua_isstring(L, -1)) {
                    target.depends.push_back(lua_tostring(L, -1));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        
        lua_getfield(L, 1, "inputs");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (lua_isstring(L, -1)) {
                    target.inputs.push_back(lua_tostring(L, -1));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        
        lua_getfield(L, 1, "outputs");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (lua_isstring(L, -1)) {
                    target.outputs.push_back(lua_tostring(L, -1));
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        
        lua_getfield(L, 1, "phony");
        if (lua_isboolean(L, -1)) {
            target.phony = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);
        
        lua_getfield(L, 1, "build");
        if (lua_isfunction(L, -1)) {
            target.lua_func_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }

        lua_getfield(L, 1, "exec");
        if (lua_isfunction(L, -1)) {
            target.on_execute_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }

        lua_getfield(L, 1, "after_exec");
        if (lua_isfunction(L, -1)) {
            target.after_execute_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        } else {
            lua_pop(L, 1);
        }
        
        forge->targets[target.name] = target;
        
        return 0;
    }
    
    static int lua_exec(lua_State* L) {
        Forge* forge = static_cast<Forge*>(lua_touserdata(L, lua_upvalueindex(1)));
        const char* cmd = luaL_checkstring(L, 1);
        
        if (forge->dry_run) {
            std::lock_guard<std::mutex> lock(forge->exec_mutex);
            std::cout << "[DRY] " << cmd << std::endl;
        } else {
            if (forge->verbose) {
                std::lock_guard<std::mutex> lock(forge->exec_mutex);
                std::cout << cmd << std::endl;
            }
            int ret = system(cmd);
            if (ret != 0) {
                std::lock_guard<std::mutex> lock(forge->exec_mutex);
                std::cerr << "Command failed with exit code " << ret << std::endl;
                forge->has_error = true;
                exit(ret);
            }
        }
        
        return 0;
    }
    
    static int lua_glob(lua_State* L) {
        const char* pattern_str = luaL_checkstring(L, 1);
        std::string pattern(pattern_str);
        std::vector<std::string> files;
        
        size_t star_pos = pattern.find("**");
        if (star_pos != std::string::npos) {
            std::string dir = pattern.substr(0, pattern.find_last_of("/", star_pos));
            std::string ext = pattern.substr(pattern.find_last_of("."));
            
            if (fs::exists(dir)) {
                for (auto& entry : fs::recursive_directory_iterator(dir)) {
                    if (entry.path().extension() == ext) {
                        files.push_back(entry.path().string());
                    }
                }
            }
        } else {
            std::string dir = ".";
            std::string search_pattern = pattern;
            
            size_t last_slash = pattern.find_last_of("/");
            if (last_slash != std::string::npos) {
                dir = pattern.substr(0, last_slash);
                search_pattern = pattern.substr(last_slash + 1);
            }
            
            if (fs::exists(dir)) {
                for (auto& entry : fs::directory_iterator(dir)) {
                    std::string filename = entry.path().filename().string();
                    bool match = true;
                    
                    size_t p_idx = 0, f_idx = 0;
                    while (p_idx < search_pattern.length() && f_idx < filename.length()) {
                        if (search_pattern[p_idx] == '*') {
                            if (p_idx + 1 >= search_pattern.length()) {
                                f_idx = filename.length();
                                break;
                            }
                            char next = search_pattern[p_idx + 1];
                            while (f_idx < filename.length() && filename[f_idx] != next) {
                                f_idx++;
                            }
                            p_idx++;
                        } else if (search_pattern[p_idx] == filename[f_idx]) {
                            p_idx++;
                            f_idx++;
                        } else {
                            match = false;
                            break;
                        }
                    }
                    
                    if (match && p_idx >= search_pattern.length() && f_idx >= filename.length()) {
                        files.push_back(entry.path().string());
                    }
                }
            }
        }
        
        lua_newtable(L);
        for (size_t i = 0; i < files.size(); i++) {
            lua_pushstring(L, files[i].c_str());
            lua_rawseti(L, -2, i + 1);
        }
        
        return 1;
    }
    
    static int lua_exists(lua_State* L) {
        const char* path = luaL_checkstring(L, 1);
        lua_pushboolean(L, fs::exists(path));
        return 1;
    }
    
    static int lua_mkdir(lua_State* L) {
        const char* path = luaL_checkstring(L, 1);
        fs::create_directories(path);
        return 0;
    }
    
    static int lua_basename(lua_State* L) {
        const char* path = luaL_checkstring(L, 1);
        lua_pushstring(L, fs::path(path).filename().string().c_str());
        return 1;
    }
    
    static int lua_dirname(lua_State* L) {
        const char* path = luaL_checkstring(L, 1);
        lua_pushstring(L, fs::path(path).parent_path().string().c_str());
        return 1;
    }

    static int lua_print(lua_State* L) {
        for (int i = 1; i <= lua_gettop(L); i++) {
            std::cout << lua_tostring(L, i) << " ";
        }
        std::cout << std::endl;
        return 0;
    }
    
    void execute_target_parallel(const std::string& name) {
        {
            std::lock_guard<std::mutex> lock(exec_mutex);
            if (executed.count(name)) return;
            if (in_progress.count(name)) return;
            in_progress.insert(name);
        }
        
        if (!targets.count(name)) {
            std::lock_guard<std::mutex> lock(exec_mutex);
            std::cerr << "Target not found: " << name << std::endl;
            has_error = true;
            exit(1);
        }
        
        Target& target = targets[name];

        if (target.on_execute_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target.on_execute_ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::lock_guard<std::mutex> lock(exec_mutex);
                std::cerr << "Lua error in on_execute: " << lua_tostring(L, -1) << std::endl;
                lua_pop(L, 1);
                has_error = true;
                exit(1);
            }
        }

        for (const auto& dep : target.depends) {
            bool dep_done = false;
            while (!dep_done && !has_error) {
                {
                    std::lock_guard<std::mutex> lock(exec_mutex);
                    dep_done = executed.count(dep) > 0;
                }
                if (!dep_done) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
        }
        
        if (has_error) {
            std::lock_guard<std::mutex> lock(exec_mutex);
            in_progress.erase(name);
            return;
        }
        
        if (!needs_rebuild(target)) {
            if (verbose) {
                std::lock_guard<std::mutex> lock(exec_mutex);
                std::cout << "==> Target '" << name << "' is up to date" << std::endl;
            }
            std::lock_guard<std::mutex> lock(exec_mutex);
            executed.insert(name);
            in_progress.erase(name);
            exec_cv.notify_all();
            return;
        }
        
        if (verbose) {
            std::lock_guard<std::mutex> lock(exec_mutex);
            std::cout << "==> Running target: " << name << std::endl;
        }
        
        if (target.lua_func_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target.lua_func_ref);
            
            lua_newtable(L);
            for (size_t i = 0; i < target.inputs.size(); i++) {
                lua_pushstring(L, target.inputs[i].c_str());
                lua_rawseti(L, -2, i + 1);
            }
            lua_setglobal(L, "INPUTS");
            
            lua_newtable(L);
            for (size_t i = 0; i < target.outputs.size(); i++) {
                lua_pushstring(L, target.outputs[i].c_str());
                lua_rawseti(L, -2, i + 1);
            }
            lua_setglobal(L, "OUTPUTS");
            
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::lock_guard<std::mutex> lock(exec_mutex);
                std::cerr << "Lua error: " << lua_tostring(L, -1) << std::endl;
                lua_pop(L, 1);
                has_error = true;
                exit(1);
            }
        }

        if (target.after_execute_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target.after_execute_ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::lock_guard<std::mutex> lock(exec_mutex);
                std::cerr << "Lua error in after_exec: " << lua_tostring(L, -1) << std::endl;
                lua_pop(L, 1);
                has_error = true;
                exit(1);
            }
        }

        {
            executed.insert(name);
            in_progress.erase(name);
            exec_cv.notify_all();
        }
    }
    
    void build_dependency_graph(const std::string& name, std::set<std::string>& all_targets) {
        if (all_targets.count(name)) return;
        if (!targets.count(name)) return;
        
        all_targets.insert(name);
        
        for (const auto& dep : targets[name].depends) {
            build_dependency_graph(dep, all_targets);
        }
    }
    
public:
    Forge() {
        L = luaL_newstate();
        luaL_openlibs(L);
        
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, lua_target, 1);
        lua_setglobal(L, "target");
        
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, lua_exec, 1);
        lua_setglobal(L, "exec");
        
        lua_pushcfunction(L, lua_glob);
        lua_setglobal(L, "glob");
        
        lua_pushcfunction(L, lua_exists);
        lua_setglobal(L, "exists");
        
        lua_pushcfunction(L, lua_mkdir);
        lua_setglobal(L, "mkdir");
        
        lua_pushcfunction(L, lua_basename);
        lua_setglobal(L, "basename");
        
        lua_pushcfunction(L, lua_dirname);
        lua_setglobal(L, "dirname");

        lua_pushcfunction(L, lua_print);
        lua_setglobal(L, "print");

        #ifdef _WIN32
        lua_pushstring(L, "windows");
        #elif __APPLE__
        lua_pushstring(L, "macos");
        #else
        lua_pushstring(L, "linux");
        #endif
        lua_setglobal(L, "OS");
    }
    
    ~Forge() {
        if (L) {
            lua_close(L);
        }
    }
    
    void parse(const std::string& filename) {
        if (luaL_dofile(L, filename.c_str()) != LUA_OK) {
            std::cerr << "Lua error: " << lua_tostring(L, -1) << std::endl;
            lua_pop(L, 1);
            exit(1);
        }
    }
    
    void execute_target(const std::string& name) {
        if (executed.count(name)) return;
        
        if (!targets.count(name)) {
            std::cerr << "Target not found: " << name << std::endl;
            exit(1);
        }
        
        Target& target = targets[name];

        if (target.on_execute_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target.on_execute_ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::cerr << "Lua error in on_execute: " << lua_tostring(L, -1) << std::endl;
                lua_pop(L, 1);
                exit(1);
            }
        }


        for (const auto& dep : target.depends) {
            execute_target(dep);
        }
        
        if (!needs_rebuild(target)) {
            if (verbose) {
                std::cout << "==> Target '" << name << "' is up to date" << std::endl;
            }
            executed.insert(name);
            return;
        }
        
        if (verbose) std::cout << "==> Running target: " << name << std::endl;
        
        if (target.lua_func_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target.lua_func_ref);
            
            lua_newtable(L);
            for (size_t i = 0; i < target.inputs.size(); i++) {
                lua_pushstring(L, target.inputs[i].c_str());
                lua_rawseti(L, -2, i + 1);
            }
            lua_setglobal(L, "INPUTS");
            
            lua_newtable(L);
            for (size_t i = 0; i < target.outputs.size(); i++) {
                lua_pushstring(L, target.outputs[i].c_str());
                lua_rawseti(L, -2, i + 1);
            }
            lua_setglobal(L, "OUTPUTS");
            
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::cerr << "Lua error: " << lua_tostring(L, -1) << std::endl;
                lua_pop(L, 1);
                exit(1);
            }
        }

        if (target.after_execute_ref != LUA_NOREF) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, target.after_execute_ref);
            if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
                std::cerr << "Lua error in after_exec: " << lua_tostring(L, -1) << std::endl;
                lua_pop(L, 1);
                has_error = true;
                exit(1);
            }
        }
        
        executed.insert(name);
    }
    
    void execute_targets_parallel(const std::vector<std::string>& target_names) {
        std::set<std::string> all_targets;
        for (const auto& name : target_names) {
            build_dependency_graph(name, all_targets);
        }
        
        std::vector<std::thread> workers;
        
        auto worker_func = [this, &all_targets]() {
            while (true) {
                std::string target_to_execute;
                
                {
                    std::unique_lock<std::mutex> lock(exec_mutex);
                    
                    exec_cv.wait(lock, [this, &all_targets]() {
                        if (has_error) return true;
                        if (executed.size() == all_targets.size()) return true;
                        
                        for (const auto& t : all_targets) {
                            if (!executed.count(t) && !in_progress.count(t) && can_execute(t)) {
                                return true;
                            }
                        }
                        return false;
                    });
                    
                    if (has_error || executed.size() == all_targets.size()) {
                        return;
                    }
                    
                    for (const auto& t : all_targets) {
                        if (!executed.count(t) && !in_progress.count(t) && can_execute(t)) {
                            target_to_execute = t;
                            break;
                        }
                    }
                }
                
                if (!target_to_execute.empty()) {
                    execute_target_parallel(target_to_execute);
                }
            }
        };
        
        for (int i = 0; i < max_jobs; i++) {
            workers.emplace_back(worker_func);
        }
        
        exec_cv.notify_all();
        
        for (auto& worker : workers) {
            worker.join();
        }
    }
    
    void set_dry_run(bool flag) { dry_run = flag; }
    void set_verbose(bool flag) { verbose = flag; }
    void set_max_jobs(int jobs) { max_jobs = jobs; }
    void set_rebuild(bool flag) { rebuild = flag; }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: forge <target> [options]" << std::endl;
        return 1;
    }
    
    Forge forge;
    bool dry_run = false;
    bool verbose = false;
    bool rebuild = false;
    int max_jobs = 1;
    std::vector<std::string> targets;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "-B") {
            rebuild = true;
        } else if (arg == "-j") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                max_jobs = std::stoi(argv[++i]);
            } else {
                max_jobs = std::thread::hardware_concurrency();
            }
        } else if (arg.substr(0, 2) == "-j" && arg.length() > 2) {
            max_jobs = std::stoi(arg.substr(2));
        } else if (arg[0] != '-') {
            targets.push_back(arg);
        }
    }
    
    forge.set_dry_run(dry_run);
    forge.set_verbose(verbose);
    forge.set_max_jobs(max_jobs);
    forge.set_rebuild(rebuild);
    
    if (!fs::exists("Forgefile")) {
        std::cerr << "Forgefile.lua not found" << std::endl;
        return 1;
    }
    
    forge.parse("Forgefile");
    
    if (max_jobs > 1) {
        forge.execute_targets_parallel(targets);
    } else {
        for (const auto& target : targets) {
            forge.execute_target(target);
        }
    }
    
    return 0;
}
