// Adapted from Exzap/ZArchive d2c717730092c7bf8cbb033b12fd4001b7c4d932 src/main.cpp.
// MIT-0; see LICENSE. Nova5: live stdout, checked IO, Unicode and long paths, no self-packing.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <crtdbg.h>
#endif
#include <exception>
#include "zarchive/zarchivewriter.h"
#include "zarchive/zarchivereader.h"

#include <vector>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <optional>

#include <stdio.h>

namespace fs = std::filesystem;

// Windows' extended-length form (\\?\C:\..., \\?\UNC\server\...): games keep files past 260 characters, such as
// God of War Sons of Sparta's Media/StreamingAssets/aa/PS5/*.bundle under a long release-folder name.
fs::path Native(const fs::path& path)
{
#ifdef _WIN32
	std::error_code ec;
	auto text = fs::absolute(path, ec).lexically_normal().wstring();
	if (text.rfind(L"\\\\?\\", 0) == 0) return text;
	if (text.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + text.substr(2));
	return fs::path(L"\\\\?\\" + text);
#else
	return path;
#endif
}

void PrintHelp()
{
	puts("Usage:\n");
	puts("zarchive.exe input_path [output_path]");
	puts("If input_path is a directory, then output_path will be the ZArchive output file path");
	puts("If input_path is a ZArchive file path, then output_path will be the output directory");
	puts("output_path is optional");
	puts("zarchive.exe archive.zar output_dir /sub/folder extracts only that folder (Nova5: a .zar's sce_sys)");
}

bool ExtractFile(ZArchiveReader* reader, std::string_view srcPath, const fs::path& path)
{
	ZArchiveNodeHandle fileHandle = reader->LookUp(srcPath, true, false);
	if (fileHandle == ZARCHIVE_INVALID_NODE)
	{
		puts("Unable to extract file:");
		puts(std::string(srcPath).c_str());
		return false;
	}

	std::vector<uint8_t> buffer;
	buffer.resize(64 * 1024);

	std::ofstream fileOut(Native(path), std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
	if (!fileOut.is_open())
	{
		puts("Unable to write file:");
		puts(reinterpret_cast<const char*>(path.generic_u8string().c_str()));
	}
	uint64_t readOffset = 0;
	while (true)
	{
		uint64_t bytesRead = reader->ReadFromFile(fileHandle, readOffset, buffer.size(), buffer.data());
		if (bytesRead == 0)
			break;
		fileOut.write((const char*)buffer.data(), bytesRead);
		if (!fileOut) return false;
		readOffset += bytesRead;
	}
	fileOut.flush();
	if (!fileOut || readOffset != reader->GetFileSize(fileHandle))
		return false;

	return true;
}

bool ExtractRecursive(ZArchiveReader* reader, std::string srcPath, fs::path outputDirectory)
{
	ZArchiveNodeHandle dirHandle = reader->LookUp(srcPath, false, true);
	if (dirHandle == ZARCHIVE_INVALID_NODE)
		return false;
	std::error_code ec;
	fs::create_directories(Native(outputDirectory), ec);
	uint32_t numEntries = reader->GetDirEntryCount(dirHandle);
	for (uint32_t i = 0; i < numEntries; i++)
	{
		ZArchiveReader::DirEntry dirEntry;
		if (!reader->GetDirEntry(dirHandle, i, dirEntry))
		{
			puts("Directory contains invalid node");
			return false;
		}
		puts(std::string(srcPath).append("/").append(dirEntry.name).c_str());
		if (dirEntry.isDirectory)
		{
			if (!ExtractRecursive(reader, std::string(srcPath).append("/").append(dirEntry.name), outputDirectory / fs::u8path(dirEntry.name))) return false;
		}
		else
		{
			// extract file
			if (!ExtractFile(reader, std::string(srcPath).append("/").append(dirEntry.name), outputDirectory / fs::u8path(dirEntry.name)))
				return false;
		}
	}
	return true;
}

int Extract(fs::path inputFile, fs::path outputDirectory, const std::string& folder = "")
{
	std::error_code ec;
	if (!fs::exists(inputFile, ec))
	{
		puts("Unable to find archive file");
		return -10;
	}

	ZArchiveReader* reader = ZArchiveReader::OpenFromFile(inputFile);
	if (!reader)
	{
		puts("Failed to open ZArchive");
		return -11;
	}
	// A folder alone ("/sce_sys"): into outputDirectory/<its name>.
	bool r = folder.empty() ? ExtractRecursive(reader, "", outputDirectory)
	                        : ExtractRecursive(reader, folder, outputDirectory / fs::u8path(folder.substr(folder.find_last_of('/') + 1)));
	if (!r)
	{
		puts("Extraction failed");
		delete reader;
		return -12;
	}
	delete reader;
	return 0;
}

struct PackContext
{
	fs::path outputFilePath;
	std::ofstream currentOutputFile;
	bool hasError{false};
};

void _pack_NewOutputFile(const int32_t partIndex, void* ctx)
{
	PackContext* packContext = (PackContext*)ctx;
	packContext->currentOutputFile = std::ofstream(Native(packContext->outputFilePath), std::ios::binary);
	if (!packContext->currentOutputFile.is_open())
	{
		printf("Failed to create output file: %s\n", reinterpret_cast<const char*>(packContext->outputFilePath.generic_u8string().c_str()));
		packContext->hasError = true;
	}
}

void _pack_WriteOutputData(const void* data, size_t length, void* ctx)
{
	PackContext* packContext = (PackContext*)ctx;
	packContext->currentOutputFile.write((const char*)data, length);
	if (!packContext->currentOutputFile) packContext->hasError = true;
}

int Pack(fs::path inputDirectory, fs::path outputFile)
{
	std::vector<uint8_t> buffer;
	buffer.resize(64 * 1024);

	std::error_code ec;
	PackContext packContext;
	packContext.outputFilePath = outputFile;
	ZArchiveWriter zWriter(_pack_NewOutputFile, _pack_WriteOutputData, &packContext);
	if (packContext.hasError)
		return -16;
	const auto root = Native(inputDirectory);
	for (auto const& dirEntry : fs::recursive_directory_iterator(root))
	{
		fs::path pathEntry = dirEntry.path().lexically_relative(root);
		if (dirEntry.is_directory())
		{
			if (!zWriter.MakeDir(reinterpret_cast<const char*>(pathEntry.generic_u8string().c_str()), false))
			{
				printf("Failed to create directory %s\n", reinterpret_cast<const char*>(pathEntry.generic_u8string().c_str()));
				return -13;
			}
		}
		else if (dirEntry.is_regular_file())
		{
			printf("Adding %s\n", reinterpret_cast<const char*>(pathEntry.generic_u8string().c_str()));
			if (!zWriter.StartNewFile(reinterpret_cast<const char*>(pathEntry.generic_u8string().c_str())))
			{
				printf("Failed to create archive file %s\n", reinterpret_cast<const char*>(pathEntry.generic_u8string().c_str()));
				return -14;
			}
			std::ifstream inputFile(dirEntry.path(), std::ios::binary);
			if (!inputFile.is_open())
			{
				printf("Failed to open input file %s\n", reinterpret_cast<const char*>(pathEntry.generic_u8string().c_str()));
				return -15;
			}
			while( true )
			{
				inputFile.read((char*)buffer.data(), buffer.size());
				int32_t readBytes = (int32_t)inputFile.gcount();
				if (readBytes <= 0)
					break;
				zWriter.AppendData(buffer.data(), readBytes);
				if (packContext.hasError) return -16;
			}
			if (inputFile.bad()) { puts("Input read failed" ); return -15; }
		}
		if (packContext.hasError)
			return -16;
	}
	zWriter.Finalize();
	packContext.currentOutputFile.flush();
	if (packContext.hasError || !packContext.currentOutputFile) { puts("Archive write failed" ); return -16; }
	puts("Archive complete" );
	return 0;
}

int ToolMain(int argc, char* argv[])
{
	if (argc <= 1)
	{
		PrintHelp();
		return 0;
	}
	std::optional<std::string> strInput;
	std::optional<std::string> strOutput;
	std::string folder;
	for (int i = 1; i < argc; i++)
	{
		if (strInput)
		{
			if (strOutput && folder.empty() && argv[i][0] == '/')
			{
				folder = argv[i];
			}
			else if (strOutput)
			{
				puts("Too many paths specified");
				return -1;
			}
			else
			{
				strOutput = argv[i];
			}
		}
		else
		{
			strInput = argv[i];
		}
	}

	if (strInput)
	{
		std::error_code ec;
		fs::path p = fs::u8path(*strInput);
		if (fs::is_regular_file(p, ec))
		{
			// extract
			fs::path outputDirectory;
			if (!strOutput)
			{
				fs::path defaultOutputPath = p.parent_path() / p.stem(); defaultOutputPath += "_extracted";
				outputDirectory = defaultOutputPath;
				printf("Extracting to: %s\n", reinterpret_cast<const char*>(outputDirectory.generic_u8string().c_str()));
			}
			else
				outputDirectory = fs::u8path(*strOutput);
			if (fs::exists(outputDirectory, ec) && !fs::is_directory(outputDirectory, ec))
			{
				puts("The specified output path is not a valid directory");
				return -3;
			}
			fs::create_directories(outputDirectory, ec);
			if (!fs::exists(outputDirectory, ec))
			{
				puts("Failed to create output directory");
				return -4;
			}
			return Extract(p, outputDirectory, folder);
		}
		else if(fs::is_directory(p, ec))
		{
			// pack
			fs::path outputFile;
			if (!strOutput)
			{
				fs::path defaultOutputPath = p.parent_path() / p.stem(); defaultOutputPath += ".zar";
				outputFile = defaultOutputPath;
				printf("Outputting to: %s\n", reinterpret_cast<const char*>(outputFile.generic_u8string().c_str()));
			}
			else
				outputFile = fs::u8path(*strOutput);
			if ((fs::exists(outputFile, ec) && !fs::is_regular_file(outputFile, ec)))
			{
				puts("The specified output path is not a valid file");
				return -10;
			}
			if ((fs::exists(outputFile, ec) && fs::is_regular_file(outputFile, ec)))
			{
				puts("The output file already exists");
				return -11;
			}
			// Creating the output inside the input tree would pack the archive into itself.
            auto inputRoot = fs::canonical(p);
            // Resolve the existing parent, not a nonexistent output file: some sandboxed
            // Windows filesystems deny canonicalization of nonexistent children.
            auto out = fs::canonical(fs::absolute(outputFile).parent_path()) / outputFile.filename();
            bool inside = false;
            for (auto parent = out.parent_path(); !parent.empty();) {
                if (fs::equivalent(inputRoot, parent, ec)) { inside = true; break; }
                auto next = parent.parent_path();
                if (next == parent) break;
                parent = next;
            }
            if (inside) { puts("Output must be outside the input directory"); return -17; }
            int r = Pack(p, outputFile);
			if (r != 0)
			{
				// delete incomplete output file
				fs::remove(outputFile, ec);
			}
			return r;
		}
		else
		{
			puts("Input path is not a valid file or directory");
			return -1;
		}
	}
	return 0;
}

int RunTool(int argc, char* argv[]) {
    // The launcher redirects stdout: flush each file immediately, without waiting for a full pipe buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    try { return ToolMain(argc, argv); }
    catch (const std::exception& e) { std::fprintf(stderr, "ZArchive error: %s\n", e.what()); return 1; }
}
#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> utf8;
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) {
        auto bytes = fs::path(argv[i]).u8string();
        utf8.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    for (auto& value : utf8) args.push_back(value.data());
    return RunTool(argc, args.data());
}
#else
int main(int argc, char* argv[]) { return RunTool(argc, argv); }
#endif
