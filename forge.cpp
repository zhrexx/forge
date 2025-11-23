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
#include <regex>

namespace fs = std::filesystem;

struct Value {
    enum Type { STRING, LIST, NONE };
    Type type = NONE;
    std::string str_val;
    std::vector<std::string> list_val;
    
    Value() = default;
    Value(const std::string& s) : type(STRING), str_val(s) {}
    Value(const std::vector<std::string>& l) : type(LIST), list_val(l) {}
    
    std::string to_string() const {
        if (type == STRING) return str_val;
        if (type == LIST && !list_val.empty()) return list_val[0];
        return "";
    }
};

struct Target {
    std::string name;
    std::vector<std::string> depends;
    std::vector<std::string> commands;
};

struct Function {
    std::vector<std::string> params;
    std::vector<std::string> body;
};

class Forge {
private:
    std::map<std::string, Value> vars;
    std::map<std::string, Target> targets;
    std::map<std::string, Function> functions;
    std::set<std::string> executed;
    bool dry_run = false;
    bool verbose = false;
    
    std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        auto end = s.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        return s.substr(start, end - start + 1);
    }
    
    std::vector<std::string> split(const std::string& s, char delim) {
        std::vector<std::string> result;
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, delim)) {
            auto trimmed = trim(item);
            if (!trimmed.empty()) {
                result.push_back(trimmed);
            }
        }
        return result;
    }
    
    std::string strip_quotes(const std::string& s) {
        if (s.length() >= 2 && s[0] == '"' && s.back() == '"') {
            return s.substr(1, s.length() - 2);
        }
        return s;
    }
    
    std::vector<std::string> parse_args(const std::string& args_str) {
        std::vector<std::string> args;
        std::string current;
        bool in_quotes = false;
        int paren_depth = 0;
        
        for (size_t i = 0; i < args_str.length(); i++) {
            char c = args_str[i];
            
            if (c == '"' && (i == 0 || args_str[i-1] != '\\')) {
                in_quotes = !in_quotes;
                current += c;
            } else if (c == '(' && !in_quotes) {
                paren_depth++;
                current += c;
            } else if (c == ')' && !in_quotes) {
                paren_depth--;
                current += c;
            } else if (c == ',' && !in_quotes && paren_depth == 0) {
                args.push_back(trim(current));
                current.clear();
            } else {
                current += c;
            }
        }
        
        if (!current.empty()) {
            args.push_back(trim(current));
        }
        
        return args;
    }
    
    std::string expand(const std::string& s, std::map<std::string, Value>& local_vars) {
        std::string result = s;
        std::regex var_regex(R"(\{([^}]+)\})");
        std::smatch match;
        
        while (std::regex_search(result, match, var_regex)) {
            std::string var_expr = match[1].str();
            std::string replacement;
            
            if (var_expr.find('.') != std::string::npos) {
                auto parts = split(var_expr, '.');
                std::string var_name = parts[0];
                std::string method = parts.size() > 1 ? parts[1] : "";
                
                Value val = local_vars.count(var_name) ? local_vars[var_name] : vars[var_name];
                
                if (method == "len()") {
                    replacement = std::to_string(val.type == Value::LIST ? val.list_val.size() : val.str_val.size());
                } else if (method == "upper()") {
                    replacement = val.to_string();
                    std::transform(replacement.begin(), replacement.end(), replacement.begin(), ::toupper);
                } else if (method == "lower()") {
                    replacement = val.to_string();
                    std::transform(replacement.begin(), replacement.end(), replacement.begin(), ::tolower);
                } else if (method == "basename()") {
                    replacement = fs::path(val.to_string()).filename().string();
                } else if (method.find("replace(") == 0) {
                    size_t start = method.find('(') + 1;
                    size_t end = method.find(')');
                    auto args = parse_args(method.substr(start, end - start));
                    if (args.size() == 2) {
                        replacement = val.to_string();
                        std::string from = strip_quotes(args[0]);
                        std::string to = strip_quotes(args[1]);
                        size_t pos = 0;
                        while ((pos = replacement.find(from, pos)) != std::string::npos) {
                            replacement.replace(pos, from.length(), to);
                            pos += to.length();
                        }
                    }
                } else if (method.find("split(") == 0) {
                    size_t start = method.find('(') + 1;
                    size_t end = method.find(')');
                    auto args = parse_args(method.substr(start, end - start));
                    if (!args.empty()) {
                        std::string delim = strip_quotes(args[0]);
                        auto parts_split = split(val.to_string(), delim[0]);
                        if (method.find("[-1]") != std::string::npos && !parts_split.empty()) {
                            replacement = parts_split.back();
                        } else if (!parts_split.empty()) {
                            replacement = parts_split[0];
                        }
                    }
                }
            } else {
                Value val = local_vars.count(var_expr) ? local_vars[var_expr] : vars[var_expr];
                if (val.type == Value::LIST) {
                    for (size_t i = 0; i < val.list_val.size(); i++) {
                        replacement += val.list_val[i];
                        if (i < val.list_val.size() - 1) replacement += " ";
                    }
                } else {
                    replacement = val.to_string();
                }
            }
            
            result.replace(match.position(), match.length(), replacement);
        }
        
        return result;
    }
    
    Value eval_builtin(const std::string& func, const std::vector<std::string>& args, std::map<std::string, Value>& local_vars) {
        if (func == "env" && args.size() == 1) {
            const char* val = std::getenv(strip_quotes(args[0]).c_str());
            return Value(val ? std::string(val) : "");
        } else if (func == "glob" && args.size() == 1) {
            std::vector<std::string> files;
            std::string pattern = expand(strip_quotes(args[0]), local_vars);
            
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
                    std::regex regex_pattern(search_pattern);
                    std::string regex_str = search_pattern;
                    size_t pos = 0;
                    while ((pos = regex_str.find('*', pos)) != std::string::npos) {
                        regex_str.replace(pos, 1, ".*");
                        pos += 2;
                    }
                    pos = 0;
                    while ((pos = regex_str.find('.', pos)) != std::string::npos) {
                        if (pos == 0 || regex_str[pos-1] != '\\') {
                            regex_str.insert(pos, "\\");
                            pos += 2;
                        } else {
                            pos++;
                        }
                    }
                    
                    try {
                        std::regex file_regex(regex_str);
                        for (auto& entry : fs::directory_iterator(dir)) {
                            std::string filename = entry.path().filename().string();
                            if (std::regex_match(filename, file_regex)) {
                                files.push_back(entry.path().string());
                            }
                        }
                    } catch (...) {
                        for (auto& entry : fs::directory_iterator(dir)) {
                            files.push_back(entry.path().string());
                        }
                    }
                }
            }
            return Value(files);
        } else if (func == "exists" && args.size() == 1) {
            return Value(fs::exists(expand(strip_quotes(args[0]), local_vars)) ? "true" : "false");
        } else if (func == "mkdir" && args.size() == 1) {
            std::string path = expand(strip_quotes(args[0]), local_vars);
            fs::create_directories(path);
            return Value("");
        } else if (func == "os") {
            #ifdef _WIN32
            return Value("windows");
            #elif __APPLE__
            return Value("macos");
            #else
            return Value("linux");
            #endif
        } else if (func == "println") {
            for (size_t i = 0; i < args.size(); i++) {
                std::string arg = strip_quotes(expand(args[i], local_vars));
                std::cout << arg;
                if (i < args.size() - 1) std::cout << " ";
            }
            std::cout << std::endl;
            return Value("");
        } else if (func == "print") {
            for (size_t i = 0; i < args.size(); i++) {
                std::string arg = strip_quotes(expand(args[i], local_vars));
                std::cout << arg;
                if (i < args.size() - 1) std::cout << " ";
            }
            return Value("");
        } else if (func == "set_verbose" && args.size() == 1) {
	    verbose = expand(strip_quotes(args[0]), local_vars) == "true" ? true : false;
	    return Value("");
	}
        
        return Value("");
    }
    
    Value eval_function_call(const std::string& line, std::map<std::string, Value>& local_vars) {
        size_t paren_pos = line.find('(');
        if (paren_pos == std::string::npos) return Value("");
        
        std::string func_name = trim(line.substr(0, paren_pos));
        size_t end_paren = line.find_last_of(')');
        std::string args_str = line.substr(paren_pos + 1, end_paren - paren_pos - 1);
        
        auto args = parse_args(args_str);
        
        std::vector<std::string> expanded_args;
        for (const auto& arg : args) {
            expanded_args.push_back(expand(arg, local_vars));
        }
        
        if (functions.count(func_name)) {
            Function& func = functions[func_name];
            std::map<std::string, Value> func_locals = local_vars;
            
            for (size_t i = 0; i < func.params.size() && i < expanded_args.size(); i++) {
                func_locals[func.params[i]] = Value(strip_quotes(expanded_args[i]));
            }
            
            execute_block(func.body, func_locals);
            return Value("");
        }
        
        return eval_builtin(func_name, args, local_vars);
    }
    
    void execute_command(const std::string& cmd, std::map<std::string, Value>& local_vars) {
        std::string expanded = expand(cmd, local_vars);
        
        if (expanded.find("mkdir ") == 0) {
            std::string path = trim(expanded.substr(6));
            if (dry_run) {
                std::cout << "[DRY] " << expanded << std::endl;
            } else {
                fs::create_directories(path);
                if (verbose) std::cout << expanded << std::endl;
            }
            return;
        }
        
        if (dry_run) {
            std::cout << "[DRY] " << expanded << std::endl;
        } else {
            if (verbose) std::cout << expanded << std::endl;
            int ret = system(expanded.c_str());
            if (ret != 0) {
                std::cerr << "Command failed with exit code " << ret << std::endl;
                exit(ret);
            }
        }
    }
    
    void execute_block(const std::vector<std::string>& commands, std::map<std::string, Value> local_vars) {
        for (size_t i = 0; i < commands.size(); i++) {
            std::string line = trim(commands[i]);
            
            if (line.empty() || line[0] == '#') continue;
            
            if (line.find("if ") == 0) {
                size_t cond_end = line.find(" {");
                std::string condition = trim(line.substr(3, cond_end - 3));
                
                Value cond_val = local_vars.count(condition) ? local_vars[condition] : vars[condition];
                bool is_true = !cond_val.to_string().empty() && cond_val.to_string() != "false" && cond_val.to_string() != "0";
                
                std::vector<std::string> if_block, else_block;
                i++;
                int depth = 1;
                bool in_else = false;
                
                while (i < commands.size() && depth > 0) {
                    std::string block_line = trim(commands[i]);
                    if (block_line == "}") {
                        depth--;
                        if (depth == 0) break;
                    } else if (block_line.find("} else {") == 0) {
                        in_else = true;
                        i++;
                        continue;
                    }
                    
                    if (in_else) {
                        else_block.push_back(commands[i]);
                    } else {
                        if_block.push_back(commands[i]);
                    }
                    i++;
                }
                
                execute_block(is_true ? if_block : else_block, local_vars);
                continue;
            }
            
            if (line.find("for ") == 0) {
                size_t in_pos = line.find(" in ");
                std::string loop_var = trim(line.substr(4, in_pos - 4));
                size_t brace_pos = line.find(" {");
                std::string collection = trim(line.substr(in_pos + 4, brace_pos - in_pos - 4));
                
                Value coll_val = local_vars.count(collection) ? local_vars[collection] : vars[collection];
                
                std::vector<std::string> loop_block;
                i++;
                int depth = 1;
                
                while (i < commands.size() && depth > 0) {
                    std::string block_line = trim(commands[i]);
                    if (block_line == "}") {
                        depth--;
                        if (depth == 0) break;
                    }
                    loop_block.push_back(commands[i]);
                    i++;
                }
                
                if (coll_val.type == Value::LIST) {
                    for (const auto& item : coll_val.list_val) {
                        std::map<std::string, Value> loop_locals = local_vars;
                        loop_locals[loop_var] = Value(item);
                        execute_block(loop_block, loop_locals);
                    }
                }
                continue;
            }
            
            if (line.find('=') != std::string::npos && line.find('(') > line.find('=')) {
                size_t eq_pos = line.find('=');
                std::string var_name = trim(line.substr(0, eq_pos));
                std::string expr = trim(line.substr(eq_pos + 1));
                
                if (expr[0] == '[') {
                    expr = expr.substr(1, expr.length() - 2);
                    auto items = parse_args(expr);
                    std::vector<std::string> list_items;
                    for (const auto& item : items) {
                        list_items.push_back(strip_quotes(expand(item, local_vars)));
                    }
                    local_vars[var_name] = Value(list_items);
                } else if (expr.find('(') != std::string::npos) {
                    local_vars[var_name] = eval_function_call(expr, local_vars);
                } else {
                    local_vars[var_name] = Value(strip_quotes(expand(expr, local_vars)));
                }
                continue;
            }
            
            if (line.find("return ") == 0) {
                continue;
            }
            
            if (line.find('(') != std::string::npos && line.find(')') != std::string::npos) {
                eval_function_call(line, local_vars);
                continue;
            }
            
            execute_command(line, local_vars);
        }
    }
    
public:
    void parse(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Cannot open " << filename << std::endl;
            exit(1);
        }
        
        std::string line;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            
            if (line.find('=') != std::string::npos && line.find("target ") != 0 && line.find("fn ") != 0) {
                size_t eq_pos = line.find('=');
                std::string var_name = trim(line.substr(0, eq_pos));
                std::string value = trim(line.substr(eq_pos + 1));
                
                if (value[0] == '[') {
                    value = value.substr(1, value.length() - 2);
                    auto items = parse_args(value);
                    std::vector<std::string> list_items;
                    for (const auto& item : items) {
                        list_items.push_back(strip_quotes(item));
                    }
                    vars[var_name] = Value(list_items);
                } else if (value.find('(') != std::string::npos) {
                    std::map<std::string, Value> empty;
                    vars[var_name] = eval_function_call(value, empty);
                } else {
                    vars[var_name] = Value(strip_quotes(value));
                }
            } else if (line.find("target ") == 0) {
                size_t name_start = 7;
                size_t name_end = line.find(" {");
                std::string target_name = trim(line.substr(name_start, name_end - name_start));
                
                Target target;
                target.name = target_name;
                
                while (std::getline(file, line)) {
                    line = trim(line);
                    if (line == "}") break;
                    
                    if (line.find("depends:") == 0) {
                        std::string deps = trim(line.substr(8));
                        target.depends = split(deps, ' ');
                    } else if (!line.empty()) {
                        target.commands.push_back(line);
                    }
                }
                
                targets[target_name] = target;
            } else if (line.find("fn ") == 0) {
                size_t name_start = 3;
                size_t name_end = line.find('(');
                std::string func_name = trim(line.substr(name_start, name_end - name_start));
                
                size_t params_start = name_end + 1;
                size_t params_end = line.find(')');
                std::string params_str = trim(line.substr(params_start, params_end - params_start));
                
                Function func;
                if (!params_str.empty()) {
                    func.params = parse_args(params_str);
                }
                
                while (std::getline(file, line)) {
                    line = trim(line);
                    if (line == "}") break;
                    if (!line.empty()) {
                        func.body.push_back(line);
                    }
                }
                
                functions[func_name] = func;
            }
        }
    }
    
    void execute_target(const std::string& name) {
        if (executed.count(name)) return;
        
        if (!targets.count(name)) {
            std::cerr << "Target not found: " << name << std::endl;
            exit(1);
        }
        
        Target& target = targets[name];

	auto start__ = std::chrono::high_resolution_clock::now(); 
	
        for (const auto& dep : target.depends) {
            execute_target(dep);
        }

	auto end__ = std::chrono::high_resolution_clock::now();
	const std::chrono::duration<double> depends_took = end__ - start__;
	vars["DEPENDENDS_EXECUTION_TIME"] = Value(std::to_string(depends_took.count()));
	
        if (verbose) std::cout << "==> Running target: " << name << std::endl;

	std::map<std::string, Value> local_vars;
        execute_block(target.commands, local_vars);
        executed.insert(name);
    }
    
    void set_dry_run(bool flag) { dry_run = flag; }
    void set_verbose(bool flag) { verbose = flag; }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: forge <target> [options]" << std::endl;
        return 1;
    }
    
    Forge forge;
    bool dry_run = false;
    bool verbose = false;
    std::vector<std::string> targets;
    
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg[0] != '-') {
            targets.push_back(arg);
        }
    }
    
    forge.set_dry_run(dry_run);
    forge.set_verbose(verbose);
    
    if (!fs::exists("Forgefile")) {
        std::cerr << "Forgefile not found" << std::endl;
        return 1;
    }
    
    forge.parse("Forgefile");
    
    for (const auto& target : targets) {
        forge.execute_target(target);
    }
    
    return 0;
}
