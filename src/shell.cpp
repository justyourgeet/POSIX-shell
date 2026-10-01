#include <cctype>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <filesystem>
#include <sys/wait.h>
#include <system_error>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <glob.h>
#include <unordered_map>
#include <algorithm>
#include <cstring>

#include "visuals.hpp"


std::unordered_map<std::string, std::string> aliases;
std::unordered_map<std::string, std::string> shell_vars;

namespace fs = std::filesystem;
int last_exit_status = 0;
volatile sig_atomic_t get_sigint = 0; 

std::string fetch_path(){
    fs::path current_path = fs::current_path();
    const char *home_env = getenv("HOME");
    fs::path home_path = home_env ? fs::path(home_env) : fs::path();
    if(!home_path.empty()){
        auto rel = fs::relative(current_path, home_path);
        std::string rel_str = rel.string();
        if(rel_str.substr(0, 2) != ".."){
            if(rel_str == "."){
                return "~";   
            }
            return "~/" + rel_str;
        }
    }
    fs::path chopped_path;
    int count = 0;
    for(const auto &path: current_path){
        if(!path.empty() && path != "/"){
            chopped_path /= path;
            count++;
            if(count == 2) break;
        }
    }
    return chopped_path.string();
}

std::string fetch_branch(){
    fs::path current_path = fs::current_path();
    while(true){
        fs::path git_path = current_path / ".git" / "HEAD";
        if(fs::exists(git_path)){
            std::ifstream head_file(git_path);
            if(head_file.is_open()){
                std::string line;
                if(std::getline(head_file,line)){
                    while(!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')){
                        line.pop_back();
                    }
                    if(line.rfind("ref: refs/heads/",0) == 0){
                        return " [" + line.substr(16) + "] ";
                    }
                    return " [" + line.substr(0,7) + "] ";
                }
            }
        }
        if(current_path == current_path.root_path() || current_path.empty())
            break;
        current_path = current_path.parent_path();
    }
    return std::string();
}

const char MASK = '\x01';

std::vector<std::string> tokenizer(const std::string &command){
    std::vector<std::string> args;
    std::string cur;
    bool in_token = false;
    char q = '\0';

    auto flush = [&]{ if(in_token){ args.push_back(cur); cur.clear(); in_token = false; } };
    auto lit = [&](char c, const char *special){
        if(strchr(special, c)) cur += MASK;
        cur += c;
    };

    for(size_t i = 0; i < command.size(); i++){
        char c = command[i];

        if(q){
            if(c == q) q = '\0';
            else lit(c, q == '\'' ? "*?|&;<>$" : "*?|&;<>");
            continue;
        }
        if(c == '"' || c == '\''){ q = c; in_token = true; continue; }
        if(c == '\\' && i + 1 < command.size()){
            lit(command[++i], "*?|&;<>$");
            in_token = true;
            continue;
        }
        if(std::isspace((unsigned char)c)){ flush(); continue; }
        if(c == '|' || c == '&' || c == ';' || c == '<' || c == '>'){
            flush();
            std::string op(1, c);
            if((c == '|' || c == '&' || c == '>') && i + 1 < command.size() && command[i+1] == c){
                op += c; i++;
            }
            args.push_back(op);
            continue;
        }
        cur += c;
        in_token = true;
    }
    flush();
    return args;
}

void unmask(std::vector<std::string> &v){
    for(auto &s : v) s.erase(std::remove(s.begin(), s.end(), MASK), s.end());
}

void cmd_cd(const std::vector<std::string> &args){
    const char *dest = args.size() < 2 ? getenv("HOME") : args[1].c_str();
    if(!dest){ std::cerr << "cd: HOME not set\n"; last_exit_status = 1; return; }
    std::error_code ec;
    fs::current_path(dest, ec);
    if(ec){ std::cerr << "cd: " << dest << ": " << ec.message() << "\n"; last_exit_status = 1; }
    else last_exit_status = 0;
}


struct Redirect{
    std::string out_file;
    bool append = false;
    std::string in_file;
};

Redirect parse_redirects(std::vector<std::string> &args){
    Redirect r;
    std::vector<std::string> clean;
    for(int i = 0; i < args.size(); i++){
        if(args[i] == ">" && i + 1 < args.size()){
            r.out_file = args[++i];
        }
        else if(args[i] == ">>" && i + 1 < args.size()){
            r.out_file = args[++i];
            r.append = true;
        }
        else if(args[i] == "<" && i + 1< args.size()){
            r.in_file = args[++i];
        }
        else{clean.push_back(args[i]);}
    }
    args = clean;
    return r;
}

void apply_redirects(const Redirect r){
    if(!r.out_file.empty()){
        int flags = O_WRONLY | O_CREAT | (r.append ? O_APPEND : O_TRUNC);
        int fd = open(r.out_file.c_str(), flags, 0644);
        if(fd < 0) {std::cerr << "faied to open"; _exit(1);}
        dup2(fd, 1);
        close(fd); 
    }
    if(!r.in_file.empty()){
        int fd = open(r.in_file.c_str(), O_RDONLY);
        if(fd < 0) {std::cerr << "failed to open";_exit(1);}
        dup2(fd, 0);
        close(fd);
    }
}

std::vector<std::vector<std::string>> parse_pipeline(const std::vector<std::string> &args){
    std::vector<std::vector<std::string>> commands;
    std::vector<std::string> current;
    for(const auto &tok: args){
        if(tok == "|"){
            if(current.empty()){
                std::cerr << "syntax error near unexpected token '|'\n";
                return {};
                }
                commands.push_back(current);
                current.clear();
                continue;
            }else{
            current.push_back(tok);
            }
        }
    if(!current.empty())
        commands.push_back(current);
    return commands;
}

void run_pipeline(std::vector<std::vector<std::string>> &commands){
    int num = commands.size();
    int prev_read = -1;
    int fd[2];
    
    std::vector<pid_t> pids;
    pid_t pgid = 0;  

    for(int i = 0; i < num; i++){
        pipe(fd);

        pid_t pid = fork();

        if(pid < 0) {std::cerr << "fork failed";}

        if(pid == 0){
            if(pgid == 0) setpgid(0, 0);
            else setpgid(0, pgid);
            signal(SIGINT, SIG_DFL);   
            signal(SIGTSTP, SIG_DFL);
            if(prev_read != -1){
                dup2(prev_read, 0);
                close(prev_read);
            }
            if(i < num - 1){
                dup2(fd[1], 1);
            }
            close(fd[0]);
            close(fd[1]);

            Redirect r = parse_redirects(commands[i]);
            unmask(commands[i]);
            apply_redirects(r);
            
            std::vector<char *> argv;
            for(const auto &a: commands[i]){
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);

            execvp(argv[0], argv.data());
            std::cerr<< "command not found \n";
            _exit(127);
        }
        else{
            if(pgid == 0) pgid = pid;
            setpgid(pid, pgid);

            pids.push_back(pid);
            if(prev_read != -1) 
                    close(prev_read);
                close(fd[1]);
                prev_read = fd[0];
            }
        }

        if(prev_read != -1) close(prev_read);
        tcsetpgrp(STDIN_FILENO, pgid);

        for(size_t i = 0; i < pids.size(); i++){
        int status;
        waitpid(pids[i], &status, 0);
        if(i == pids.size() - 1){
        if(WIFEXITED(status)){
            last_exit_status = WEXITSTATUS(status);
        }
        else if(WIFSIGNALED(status)){
            last_exit_status = 128 + WTERMSIG(status);
        }
    }
}
tcsetpgrp(STDIN_FILENO, getpid());
}

    void handle_signal(int sig){ 
            get_sigint = 1;
    }

    int check_sigint(){
    if(get_sigint){ 
        get_sigint = 0; 
        std::cout << "\n";
        std::cout.flush();
        rl_free_line_state();  
        rl_cleanup_after_signal();   
        rl_on_new_line();  
        rl_replace_line("", 0); 
        rl_redisplay(); 
    }
    return 0;
}

struct Job{
    pid_t pid;
    std::string command;
    bool stopped;
};
std::vector<Job> jobs;

void cmd_jobs(){
    for(size_t i = 0; i < jobs.size(); i++){
    std::cout << "[" << i + 1 << "]  ±  "
        << (jobs[i].stopped ? "suspended" : "running") << "  "
        << jobs[i].command << "\n";
    }
}

void cmd_fg(const std::vector<std::string> &args){
    if(jobs.empty()){std::cerr << "fg; no current jobs \n"; return;}

    int idx = jobs.size() - 1;
    if(args.size() >= 2){idx = std::stoi(args[1]) - 1;}
    if(idx < 0 || idx >= (int)jobs.size()) {std::cerr << "fg: no such jobs \n"; return;}

    Job j = jobs[idx];
    jobs.erase(jobs.begin() + idx);

    std::cout << "[" << idx + 1 << "]  ±  " << j.pid << "  continued  " << j.command << "\n";
    tcsetpgrp(STDIN_FILENO, j.pid);
    kill(j.pid, SIGCONT);

    int status;
    waitpid(j.pid,&status,WUNTRACED);
    tcsetpgrp(STDIN_FILENO, getpid());
    if(WIFSTOPPED(status)){
    jobs.push_back({j.pid, j.command, true});
    std::cout << "\n[" << idx + 1 << "]  ±  " << j.pid << "  suspended  " << j.command << "\n";
    }

}
void cmd_bg(const std::vector<std::string> &args){
    if(jobs.empty()) {std::cerr << "bg: no such jobs \n"; return;}

    int idx = jobs.size() - 1;
    if(args.size() >= 2) idx = std::stoi(args[1]) - 1;
    if(idx < 0 || idx >= (int)jobs.size()) {std::cerr << "bg: no such jobs \n"; return;}

    jobs[idx].stopped = false;
    kill(jobs[idx].pid, SIGCONT);

    std::cout << "[" << idx + 1 << "]  ±  " << jobs[idx].pid << "  continued  " << jobs[idx].command << "\n";
}

std::vector<std::string> get_executables(const std::string &prefix){
    std::vector<std::string> matches;
    const char *path_exec = getenv("PATH");
    if(!path_exec) return matches;

    std::stringstream ss(path_exec);
    std::string dir;
    while(std::getline(ss,dir,':')){
        std::error_code ec;
        if(!fs::exists(dir,ec) || !fs::is_directory(dir,ec)) continue;;
        for(const auto &entry: fs::directory_iterator(dir,ec)){
            if(ec) break;
            std::string name = entry.path().filename().string();
            if(name.compare(0,prefix.size(),prefix) == 0){
                matches.push_back(name);
            }
        }
    }
    return matches;
}

char *command_generator(const char *text, int state){
    static std::vector<std::string> matches;
    static size_t idx;

    if(state == 0){
        idx = 0;
        matches.clear();
        std::string prefix(text);
    
        static std::vector<std::string> builtins = {"cd", "jobs", "fg", "bg", "exit", "quit"};
        for(const auto &b: builtins){
            if(b.compare(0,prefix.size(),prefix) == 0)
            matches.push_back(b);
        }
        auto path_matches = get_executables(prefix);
        matches.insert(matches.end(), path_matches.begin(), path_matches.end());
    }

    if(idx < matches.size()){
        return strdup(matches[idx++].c_str());
    }
    return nullptr;
}

char **shell_completion(const char *text, int start, int end){
    if(start == 0){
        return rl_completion_matches(text, command_generator);
    }
    return nullptr;
}

void display_matches_with_gap(char **matches, int num_matches, int max_length){
    rl_display_match_list(matches, num_matches, max_length);   
    std::cout << "\n";                                         
    rl_forced_update_display();                                
}

std::string expand_env(const std::string &token){
    std::string result;
    for(size_t i = 0; i < token.size(); i++){
        if(token[i] == MASK && i + 1 < token.size()){
            result += token[i];
            result += token[i+1];
            i++;
            continue;
        }
        if(token[i] == '$' && i + 1 < token.size()){
            if(token[i+1] == '?'){
                result += std::to_string(last_exit_status);
                i++;
                continue;
            }
            size_t j = i + 1;
            std::string var_name;
            while(j < token.size() && (isalnum((unsigned char)token[j]) || token[j] == '_')){
                var_name += token[j];
                j++;
            }
            if(!var_name.empty()){
                auto it = shell_vars.find(var_name);
                if(it != shell_vars.end()){
                    result += it->second;
                } else {
                    const char *val = getenv(var_name.c_str());
                    if(val) result += val;
                }
                i = j - 1;
                continue;
            }
        }
        result += token[i];
    }
    return result;
}

std::vector<std::string> expand_glob(const std::string &token){
    if(token.find(MASK) != std::string::npos) return {token};
    if(token.find('*') == std::string::npos && token.find('?') == std::string::npos){
        return {token};
    }
    glob_t glob_result;
    int ret = glob(token.c_str(), GLOB_NOCHECK, nullptr, &glob_result);

    std::vector<std::string> matches;
    if(ret == 0){
    for(size_t i = 0; i < glob_result.gl_pathc; i++){
        matches.push_back(glob_result.gl_pathv[i]);
    }
    }
    else{
        matches.push_back(token);
    }
    globfree(&glob_result);
    return matches;
}

void cmd_alias(const std::vector<std::string> &args){
    if(args.size() < 2){
        for(const auto &[k,v]: aliases)
        std::cout << "alias " << k << "=" << v << "\n";
    return;
    }

    std::string joined = args[1];
    for(size_t i = 2; i < args.size(); i++) joined += " " + args[i];
    
    size_t eq = joined.find('=');
    if(eq == std::string::npos){
        auto it = aliases.find(joined);
        if(it != aliases.end())
            std::cout << "alias " << it->first << "=" << it->second << "\n" << std::endl;
        else
            std::cerr << "allias: " << joined << " not found\n" << std::endl;
        return;
    }
    std::string name = joined.substr(0,eq);
    std::string value = joined.substr(eq + 1);
    aliases[name] = value;
}

void cmd_unaliase(const std::vector<std::string> &args){
    if(args.size() < 2) {std::cerr << "unalias usgae: unalias name \n"; return;}
    aliases.erase(args[1]);
}

void expand_alias(std::vector<std::string> &args){
    if(args.empty()) return;
    auto it = aliases.find(args[0]);
    if(it == aliases.end()) return;

    std::vector<std::string> alias_token = tokenizer(it -> second);
    std::vector<std::string> new_args = alias_token;
    new_args.insert(new_args.end(), args.begin() + 1, args.end());
    args = new_args;
}

struct ChainSegment{
    std::vector<std::string> tokens;
    std::string op;
};

std::vector<ChainSegment> parse_chain(const std::vector<std::string> &args){
    std::vector<ChainSegment> segments;
    std::vector<std::string> current;

    for(size_t i = 0; i < args.size(); i++){
        const std::string &tok = args[i];
        if(tok == "&&" || tok == "||" || tok == ";"){
            if(current.empty()){
                std::cerr << "syntax error near unexpected token " << tok << "\n";
                return {};
            }
            segments.push_back({current, tok});
            current.clear();
        }
        else{
            current.push_back(tok);
        }
    }
    if(!current.empty()){
        segments.push_back({current, ""});
    }
    else if(!segments.empty()){
        std::cerr << "syntax error: trailing operator \n";
        return {};
    }
    return segments;
}

bool try_assign_var(const std::string &token){
    size_t eq = token.find('=');
    if(eq == std::string::npos || eq == 0) return false;
    std::string name = token.substr(0, eq);
    for(char c : name){
        if(!isalnum((unsigned char)c) && c != '_') return false;
    }
    std::string value = token.substr(eq + 1);
    shell_vars[name] = value;
    return true;
}

void run_external(std::vector<std::string> &args){
    bool bg = false;
    if(!args.empty() && args.back() == "&"){
        bg = true;
        args.pop_back();
    }
    Redirect r = parse_redirects(args);
    unmask(args);
    if(args.empty()) return;
    std::vector<char *> argv;
    for(const auto &a: args){
        argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);

    pid_t pid = fork();

    if(pid < 0) {std::cerr << "fork failed \n"; return; }

    if(pid == 0){
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        apply_redirects(r);
        execvp(argv[0], argv.data());
        std::cerr << "command not found " << "\n";
        _exit(127);
    }
    setpgid(pid, pid);  
    if(bg){
        jobs.push_back({pid, args[0], false});
        std::cout << " [" << jobs.size() << "] " << pid << std::endl;
        return; 
    }
    tcsetpgrp(STDIN_FILENO, pid); 

    int status;
    waitpid(pid, &status, WUNTRACED); 
    tcsetpgrp(STDIN_FILENO, getpid()); 

    if(WIFEXITED(status)){
    last_exit_status = WEXITSTATUS(status);
    }

    if(WIFSTOPPED(status)){
        jobs.push_back({pid, args[0], true});
        std::cout << "\n[" << jobs.size() << "]  ±  "
        << pid << "  " << "suspended" << "  " << args[0] << "\n";
    }
    else if(WIFSIGNALED(status)){
        last_exit_status = 128 + WTERMSIG(status);
    }
}

void run_if(std::vector<std::string> &condition_args);
void run_for(std::vector<std::string> &header_args);

bool looks_like_assignment(const std::string &token){
    size_t eq = token.find('=');
    if(eq == std::string::npos || eq == 0) return false;
    std::string name = token.substr(0, eq);
    for(char c : name){
        if(!isalnum((unsigned char)c) && c != '_') return false;
    }
    return true;
}

bool run_builtin(std::vector<std::string> a){
    unmask(a);
    if(a.empty()) return false;
    if(a[0] == "cd"){ cmd_cd(a); return true; }
    if(a[0] == "jobs"){ cmd_jobs(); last_exit_status = 0; return true; }
    if(a[0] == "fg"){ cmd_fg(a); return true; }
    if(a[0] == "bg"){ cmd_bg(a); return true; }
    if(a[0] == "alias"){ cmd_alias(a); last_exit_status = 0; return true; }
    if(a[0] == "unalias"){ cmd_unaliase(a); last_exit_status = 0; return true; }
    return false;
}

void run_line(std::vector<std::string> args){
    if(args.empty()) return;

    if(args[0] == "if"){
        std::vector<std::string> cond(args.begin() + 1, args.end());
        run_if(cond);
        return;
    }
    if(args[0] == "for"){ run_for(args); return; }

    if(args.size() == 1 && looks_like_assignment(args[0])){
        try_assign_var(args[0]);
        return;
    }
    expand_alias(args);
    for(auto &a : args) a = expand_env(a);

    std::vector<std::string> expanded_args;
    for(const auto &a : args){
        auto matches = expand_glob(a);
        expanded_args.insert(expanded_args.end(), matches.begin(), matches.end());
    }
    args = expanded_args;
    if(args.empty()) return;

    std::vector<ChainSegment> chain = parse_chain(args);
    bool should_run = true;
    for(const auto &seg : chain){
        if(!should_run){ should_run = true; continue; }
        std::vector<std::vector<std::string>> commands = parse_pipeline(seg.tokens);
        if(commands.empty()) continue;
        std::vector<std::string> seg_args = seg.tokens;

        if(commands.size() == 1 && run_builtin(seg_args)) {}
        else if(commands.size() == 1) run_external(seg_args);
        else run_pipeline(commands);

        if(seg.op == "&&" && last_exit_status != 0) should_run = false;
        else if(seg.op == "||" && last_exit_status == 0) should_run = false;
    }
}

std::vector<std::string> read_block(const std::string &end_keyword){
    std::vector<std::string> lines;
    while(true){
        char *line = readline("> ");
        if(!line) break;
        std::string l = line;
        free(line);
        if(l == end_keyword) break;
        lines.push_back(l);
    }
    return lines;
}

void run_if(std::vector<std::string> &condition_args){
    std::vector<std::string> body = read_block("fi");

    std::vector<std::string> then_lines, else_lines;
    bool in_else = false;
    for(const auto &l : body){
        if(l == "else"){ in_else = true; continue; }
        if(in_else) else_lines.push_back(l);
        else then_lines.push_back(l);
    }

    run_line(condition_args);
    bool condition_true = (last_exit_status == 0);

    const auto &chosen = condition_true ? then_lines : else_lines;
    for(const auto &l : chosen){
        if(l == "then") continue;
        std::vector<std::string> toks = tokenizer(l);
        run_line(toks);
    }
}

void run_for(std::vector<std::string> &header_args){
    if(header_args.size() < 4 || header_args[2] != "in"){
        std::cerr << "for: syntax error, expected: for VAR in LIST\n";
        return;
    }
    std::string var_name = header_args[1];
    std::vector<std::string> items(header_args.begin() + 3, header_args.end());

    std::vector<std::string> body = read_block("done");

    for(const auto &item : items){
        shell_vars[var_name] = item;
        for(const auto &l : body){
            if(l == "do") continue;
            std::vector<std::string> toks = tokenizer(l);
            run_line(toks);
        }
    }
}

int main(){
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);

    rl_catch_signals = 0;
    rl_event_hook = check_sigint;

    pid_t shell_pid = getpid();
    setpgid(shell_pid, shell_pid);

    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);

    tcsetpgrp(STDIN_FILENO, shell_pid);
    print_splash();
    std::string command;
    rl_attempted_completion_function = shell_completion;
    rl_variable_bind("show-all-if-ambiguous", "on");
    rl_completion_query_items = -1;
    rl_completion_display_matches_hook = display_matches_with_gap;
    while(command != "quit" && command != "exit"){
        for(auto it = jobs.begin(); it !=jobs.end();){
            int status;
            pid_t res = waitpid(it->pid, &status, WNOHANG); 
            if(res > 0){
                int job_num = std::distance(jobs.begin(), it) + 1;
            std::cout << "[" << job_num << "]  ±  " << it->pid << "  done  " << it->command << "\n";
            it = jobs.erase(it);
            }else{
                ++it;
            }
        }
        std::string prompt = build_prompt(fetch_path(), fetch_branch(), last_exit_status);
        char *line = readline(prompt.c_str());
        if(!line){
            std::cout << "\n";
            break;
        }
        command = line;
        if(!command.empty())
            add_history(line);
        free(line);
        if(command.empty()) continue;
        std::vector<std::string> args = tokenizer(command);
        if(args.empty()) continue;
        if(args[0] == "exit" || args[0] == "quit") break;
        run_line(args);
    }
    return 0;
}
