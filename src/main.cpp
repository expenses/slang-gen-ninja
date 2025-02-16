#include "pch.h"

void diagnoseIfNeeded(slang::IBlob *diagnosticsBlob) {
    if (diagnosticsBlob != nullptr) {
        std::cout << (const char *)diagnosticsBlob->getBufferPointer()
                  << std::endl;
    }
}

Slang::ComPtr<slang::IComponentType>
compose(Slang::ComPtr<slang::ISession> &session,
        std::vector<slang::IComponentType *> &components) {
    Slang::ComPtr<slang::IComponentType> composedProgram;
    {
        Slang::ComPtr<slang::IBlob> diagnosticsBlob;
        SlangResult result = session->createCompositeComponentType(
            components.data(), components.size(), composedProgram.writeRef(),
            diagnosticsBlob.writeRef());
        diagnoseIfNeeded(diagnosticsBlob);
        if (result) {
            throw result;
        }
    }

    return composedProgram;
}

Slang::ComPtr<slang::IBlob>
link_and_compile(Slang::ComPtr<slang::IComponentType> &composedProgram) {
    Slang::ComPtr<slang::IComponentType> linkedProgram;
    {
        Slang::ComPtr<slang::IBlob> diagnosticsBlob;
        SlangResult result = composedProgram->link(linkedProgram.writeRef(),
                                                   diagnosticsBlob.writeRef());
        diagnoseIfNeeded(diagnosticsBlob);
        if (result) {
            throw result;
        }
    }

    Slang::ComPtr<slang::IBlob> spirvCode;
    {
        Slang::ComPtr<slang::IBlob> diagnosticsBlob;
        SlangResult result = linkedProgram->getTargetCode(
            0, spirvCode.writeRef(), diagnosticsBlob.writeRef());
        diagnoseIfNeeded(diagnosticsBlob);
        if (result) {
            throw result;
        }
    }

    return spirvCode;
}

std::array<uint8_t, 20> read_hash(slang::IBlob *hash) {
    std::array<uint8_t, 20> array;
    std::memcpy(array.data(), hash->getBufferPointer(), hash->getBufferSize());
    return array;
}

bool check_hash_of_file(std::filesystem::path& hash_filepath, slang::IBlob* calculated_hash) {
    std::ifstream hash_file(hash_filepath, std::ios::binary);
    if (!hash_file) {
        return false;
    }

    hash_file.seekg(0, std::ios::end);
    const auto file_size = hash_file.tellg();
    if (size_t(file_size) != calculated_hash->getBufferSize()) {
        return false;
    }
    hash_file.seekg(0);

    std::vector<uint8_t> hash_bytes(file_size);
    hash_file.read(reinterpret_cast<char*>(hash_bytes.data()), file_size);

    // Check for read failure (I think)
    if (!hash_file) {
        return false;
    }

    if (std::memcmp(hash_bytes.data(), hash_bytes.data(), file_size) != 0) {
        return false;
    }

    return true;
}

void write_bytes_to_filepath(std::filesystem::path& filepath, slang::IBlob* bytes) {
    std::ofstream output_file(filepath);
    if (!output_file) {
        std::cout << "Failed to create" << std::endl;
    }
    output_file.write((char *)bytes->getBufferPointer(),
                      bytes->getBufferSize());
    if (!output_file) {
        std::cout << "Failed to write" << std::endl;
    }
}

void process_module(Slang::ComPtr<slang::ISession> &session,
                    const std::string &name,
                    const std::filesystem::path &output_dir) {
    // 3. Load module

    Slang::ComPtr<slang::IModule> module;
    {
        Slang::ComPtr<slang::IBlob> diagnosticsBlob;
        module = session->loadModule(name.data(), diagnosticsBlob.writeRef());
        diagnoseIfNeeded(diagnosticsBlob);
        if (!module) {
            return;
        }
    }

    // auto num_deps = module->getDependencyFileCount();
    // for (auto i = 0; i < num_deps; i++) {
    //     std::cout << module->getDependencyFilePath(i) << std::endl;
    // }

    auto num_entry_points = module->getDefinedEntryPointCount();

    if (num_entry_points == 0) {
        // std::cout << "Module '" << name << "' has no entry points" <<
        // std::endl;
        return;
    }

    std::vector<slang::IComponentType *> components;
    components.reserve(num_entry_points + 1);
    components.push_back(module);

    for (auto i = 0; i < num_entry_points; i++) {
        Slang::ComPtr<slang::IEntryPoint> entry_point;
        auto result = module->getDefinedEntryPoint(i, entry_point.writeRef());
        if (result) {
            throw result;
        }
        components.push_back(entry_point);
    }

    auto composedProgram = compose(session, components);

    Slang::ComPtr<slang::IBlob> hash;
    composedProgram->getEntryPointHash(0, 0, hash.writeRef());

    std::filesystem::create_directories(output_dir);

    auto hash_filename = (output_dir / name).replace_extension("hash");

    if (check_hash_of_file(hash_filename, hash)) {
        //std::cout << "early exit" << std::endl;
        return;
    }

    auto spirvCode = link_and_compile(composedProgram);

    write_bytes_to_filepath((output_dir / name).replace_extension("spv"), spirvCode);
    write_bytes_to_filepath(hash_filename, hash);
}

void emit_build(std::filesystem::path& output_filename, slang::IModule* module, const char* filename, int index, const char* ext) {
    auto filepath = module->getDependencyFilePath(index);

    std::cout << "build " << (output_filename / filename).replace_extension(ext).c_str() << ": slang " << filepath;

    auto num_deps = module->getDependencyFileCount();


    for (auto i = index+1; i < num_deps; i++) {
        filepath = module->getDependencyFilePath(i);
        auto path = output_filename / std::filesystem::path(filepath).filename().replace_extension("slang-module");
        std::cout << " | " << path.c_str();
    }

    std::cout << std::endl;
}

int main(int argc, char **argv) {
    argparse::ArgumentParser program("slang-cacher");
    program.add_argument("shader-files")
        .nargs(argparse::nargs_pattern::at_least_one);
    program.add_argument("-o", "--output").required();

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception &err) {
        std::cerr << err.what() << std::endl;
        std::cerr << program;
        return 1;
    }

    auto output_filename =
        std::filesystem::path(program.get<std::string>("--output"));
    auto shader_files = program.get<std::vector<std::string>>("shader-files");

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

    auto search_path_strings =
        search_paths |
        std::views::transform([](auto &dir) { return dir.c_str(); }) |
        std::ranges::to<std::vector>();


    // 1. Create Global Session
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    createGlobalSession(globalSession.writeRef());

    // 2. Create Session
    slang::SessionDesc sessionDesc = {};
    slang::TargetDesc targetDesc = {};
    targetDesc.format = SLANG_SPIRV;

    sessionDesc.targets = &targetDesc;
    sessionDesc.targetCount = 1;
    sessionDesc.searchPaths = search_path_strings.data();
    sessionDesc.searchPathCount = search_path_strings.size();

    std::array<slang::CompilerOptionEntry, 1> options = {
        {slang::CompilerOptionName::Optimization,
            {slang::CompilerOptionValueKind::Int, SLANG_OPTIMIZATION_LEVEL_MAXIMAL,
            0, nullptr, nullptr}}};
    sessionDesc.compilerOptionEntries = options.data();
    sessionDesc.compilerOptionEntryCount = options.size();

    Slang::ComPtr<slang::ISession> session;
    globalSession->createSession(sessionDesc, session.writeRef());

    std::unordered_set<const char*> seen;

    auto args = "-O3 -warnings-as-errors all";

    std::cout << "rule slang" << std::endl;
    std::cout << "    command = slangc $in -o $out -I " << output_filename.c_str() << " " << args << std::endl;

    for (auto& filename: filenames) {
        Slang::ComPtr<slang::IModule> module;
        {
            Slang::ComPtr<slang::IBlob> diagnosticsBlob;
            module = session->loadModule(filename.data(), diagnosticsBlob.writeRef());
            diagnoseIfNeeded(diagnosticsBlob);
            if (!module) {
                continue;
            }
        }

        auto num_entry_points = module->getDefinedEntryPointCount();
        auto num_deps = module->getDependencyFileCount();

        if (num_deps == 0) {
            continue;
        }

        auto filepath = module->getDependencyFilePath(0);

        if (seen.contains(filepath)) {
            continue;
        }

        seen.insert(filepath);

        for (auto i = 1; i < num_deps; i++) {
            auto filepath = module->getDependencyFilePath(i);

            if (seen.contains(filepath)) {
                break;
            }

            seen.insert(filepath);

            emit_build(output_filename, module, std::filesystem::path(filepath).filename().c_str(), i, "slang-module");
        }

        auto ext = (num_entry_points > 0) ? "spv" : "slang-module";

        emit_build(output_filename, module, filename.c_str(), 0, ext);
    }
}
