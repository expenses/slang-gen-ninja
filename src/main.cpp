#include "pch.h"

void diagnoseIfNeeded(slang::IBlob *diagnosticsBlob) {
    if (diagnosticsBlob != nullptr) {
        std::cout << (const char *)diagnosticsBlob->getBufferPointer()
                  << std::endl;
    }
}

void emit_build_line(std::ofstream &ninja_file,
                     std::filesystem::path &build_dir, slang::IModule *module,
                     const char *filename,
                     std::filesystem::path &build_script_dir, int index,
                     std::string_view ext) {
    auto filepath = std::filesystem::path(module->getDependencyFilePath(index));

    if (filepath.extension() == ".slangh") {
        return;
    }

    ninja_file << "build "
               << (build_dir / filename).replace_extension(ext).c_str()
               << ": slang "
               << std::filesystem::relative(filepath, build_script_dir).c_str();

    auto num_deps = module->getDependencyFileCount();

    auto write_any_dep = false;

    for (auto i = index + 1; i < num_deps; i++) {
        filepath = module->getDependencyFilePath(i);
        if (filepath.extension() == ".slangh") {
            continue;
        }
        if (!write_any_dep) {
            ninja_file << " |";
            write_any_dep = true;
        }
        auto path =
            build_dir / filepath.filename().replace_extension("slang-module");
        ninja_file << " " << path.c_str();
    }
    ninja_file << std::endl;
}

int main(int argc, char **argv) {
    argparse::ArgumentParser program("slang-gen-ninja");
    program.add_argument("shader-files")
        .nargs(argparse::nargs_pattern::at_least_one);
    program.add_argument("-o", "--output").required();
    program.add_argument("-b", "--build-dir").required();
    program.add_argument("-f", "--filetypes")
        .nargs(argparse::nargs_pattern::at_least_one)
        .default_value(std::vector<std::string>{std::string("spv")});
    program.add_argument("-I").append();
    program.add_argument("-D").append();
    program.add_argument("-m", "--use-modules")
        .default_value(false)
        .implicit_value(true);

    // Find the position of "--" if it exists
    auto extra_args_iterator =
        std::find_if(argv, argv + argc,
                     [](const char *arg) { return strcmp(arg, "--") == 0; });

    auto program_argc = extra_args_iterator - argv;

    try {
        program.parse_args(program_argc, argv);
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    auto output_filename =
        std::filesystem::path(program.get<std::string>("--output"));
    auto shader_files = program.get<std::vector<std::string>>("shader-files");

    auto build_script_dir = output_filename.parent_path();

    auto build_dir = std::filesystem::relative(
        std::filesystem::path(program.get<std::string>("--build-dir")),
        build_script_dir);

    auto filetypes = program.get<std::vector<std::string>>("--filetypes");

    auto includes = program.get<std::vector<std::string>>("-I");
    auto defines = program.get<std::vector<std::string>>("-D");

    auto split_defines =
        defines | std::views::transform([](auto &define) {
            auto pos = define.find("=");
            return std::make_pair(std::string(define.substr(0, pos)),
                                  std::string(define.substr(pos + 1)));
        }) |
        std::ranges::to<std::vector>();

    std::vector<slang::PreprocessorMacroDesc> slang_defines =
        split_defines | std::views::transform([](auto &pair) {
            const auto &[name, value] = pair;
            return slang::PreprocessorMacroDesc{.name = name.data(),
                                                .value = value.data()};
        }) |
        std::ranges::to<std::vector>();

    std::string args = "";

    if (extra_args_iterator != argv + argc) {
        extra_args_iterator++;

        while (extra_args_iterator != argv + argc) {
            args += *extra_args_iterator;
            args += " ";
            extra_args_iterator++;
        }
    }

    std::unordered_set<std::filesystem::path> search_paths;
    std::vector<std::string> filenames;

    for (auto &shader_file : shader_files) {
        std::filesystem::path filepath(shader_file);

        if (std::filesystem::is_directory(filepath)) {
            continue;
        }

        search_paths.insert(filepath.parent_path());

        auto filename = filepath.filename();
        filename.replace_extension("");
        filenames.push_back(filename);
    }

    for (auto &include : includes) {
        search_paths.insert(std::filesystem::path(include));
    }

    auto search_path_strings =
        search_paths |
        std::views::transform([](auto &dir) { return dir.c_str(); }) |
        std::ranges::to<std::vector>();

    // 1. Create Global Session
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    createGlobalSession(globalSession.writeRef());

    // 2. Create Session
    slang::SessionDesc sessionDesc = {};
    sessionDesc.searchPaths = search_path_strings.data();
    sessionDesc.searchPathCount = search_path_strings.size();
    sessionDesc.preprocessorMacros = slang_defines.data();
    sessionDesc.preprocessorMacroCount = slang_defines.size();

    Slang::ComPtr<slang::ISession> session;
    globalSession->createSession(sessionDesc, session.writeRef());

    std::unordered_set<const char *> seen;

    auto ninja_file = std::ofstream(output_filename);

    ninja_file << "rule slang" << std::endl;
    ninja_file << "    command = slangc $in -o $out " << args;
    for (auto &include : includes) {
        ninja_file
            << " " << "-I"
            << std::filesystem::relative(include, build_script_dir).c_str();
    }
    if (program.get<bool>("--use-modules")) {
        ninja_file << " -I" << build_dir.c_str();
    }
    for (auto &define : defines) {
        ninja_file << " -D" << define;
    }
    ninja_file << std::endl;

    if (!ninja_file) {
        std::cout << "Failed to create " << output_filename << std::endl;
        return 1;
    }

    for (auto &filename : filenames) {
        Slang::ComPtr<slang::IModule> module;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;
            module = session->loadModule(filename.data(),
                                         diagnosticsBlob.writeRef());
            diagnoseIfNeeded(diagnosticsBlob);
            if (!module) {
                continue;
            }
        }

        auto num_entry_points = module->getDefinedEntryPointCount();

        if (num_entry_points == 0) {
            continue;
        }

        auto num_deps = module->getDependencyFileCount();

        if (num_deps == 0) {
            continue;
        }

        auto filepath = module->getDependencyFilePath(0);

        if (seen.contains(filepath)) {
            continue;
        }

        seen.insert(filepath);

        // Emit the build lines for all dependencies.
        for (auto i = 1; i < num_deps; i++) {
            auto filepath = module->getDependencyFilePath(i);

            if (seen.contains(filepath)) {
                break;
            }

            seen.insert(filepath);

            auto filename = std::filesystem::path(filepath).filename();

            emit_build_line(ninja_file, build_dir, module, filename.c_str(),
                            build_script_dir, i, "slang-module");
        }

        for (auto &filetype : filetypes) {
            emit_build_line(ninja_file, build_dir, module, filename.c_str(),
                            build_script_dir, 0, filetype);
        }
    }
}
