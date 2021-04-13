/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * @author Lefteris <lefteris@ethdev.com>
 * @author Gav Wood <g@ethdev.com>
 * @date 2014
 * Solidity command line interface.
 */
#include <solc/CommandLineInterface.h>

#include "solidity/BuildInfo.h"
#include "license.h"

#include <libsolidity/interface/Version.h>
#include <libsolidity/parsing/Parser.h>
#include <libsolidity/ast/ASTJsonConverter.h>
#include <libsolidity/ast/ASTJsonImporter.h>
#include <libsolidity/analysis/NameAndTypeResolver.h>
#include <libsolidity/interface/CompilerStack.h>
#include <libsolidity/interface/StandardCompiler.h>
#include <libsolidity/interface/DebugSettings.h>

#include <liblangutil/Exceptions.h>
#include <liblangutil/Scanner.h>
#include <liblangutil/SourceReferenceFormatter.h>
#include <liblangutil/SourceReferenceFormatterHuman.h>

#include <libsolutil/Common.h>
#include <libsolutil/CommonData.h>
#include <libsolutil/CommonIO.h>
#include <libsolutil/JSON.h>

#include <memory>

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/algorithm/string.hpp>

#ifdef _WIN32 // windows
	#include <io.h>
	#define isatty _isatty
	#define fileno _fileno
#else // unix
	#include <unistd.h>
#endif

#include <string>
#include <iostream>
#include <fstream>

#include <libsolidity/codegen/TVMOptimizations.hpp>

#if !defined(STDERR_FILENO)
	#define STDERR_FILENO 2
#endif

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::langutil;

namespace po = boost::program_options;

namespace solidity::frontend
{

bool g_hasOutput = false;

std::ostream& sout()
{
	g_hasOutput = true;
	return cout;
}

std::ostream& serr(bool _used = true)
{
	if (_used)
		g_hasOutput = true;
	return cerr;
}

#define cout
#define cerr

static string const g_strAstJson = "ast-json";
static string const g_strAstCompactJson = "ast-compact-json";
static string const g_strHelp = "help";
static string const g_strInputFile = "input-file";
static string const g_strLicense = "license";
static string const g_strNatspecDev = "devdoc";
static string const g_strNatspecUser = "userdoc";
static string const g_strOutputDir = "output-dir";
static string const g_strFile = "file";

static string const g_strVersion = "version";
static string const g_argAstCompactJson = g_strAstCompactJson;
static string const g_argAstJson = g_strAstJson;
static string const g_argHelp = g_strHelp;
static string const g_argInputFile = g_strInputFile;
static string const g_argNatspecDev = g_strNatspecDev;
static string const g_argNatspecUser = g_strNatspecUser;
static string const g_argOutputDir = g_strOutputDir;
static string const g_argFile = g_strFile;
static string const g_argVersion = g_strVersion;

static string const g_argDebug = "debug";
static string const g_argSetContract = "contract";
static string const g_argTvm = "tvm";
static string const g_argTvmABI = "tvm-abi";
static string const g_argTvmOptimize = "tvm-optimize";
static string const g_argTvmPeephole = "tvm-peephole";
static string const g_argTvmUnsavedStructs = "tvm-unsaved-structs";


static void version()
{
	sout() <<
		"solc, the solidity compiler commandline interface" <<
		endl <<
		"Version: " <<
		solidity::frontend::VersionString <<
		endl;
	exit(0);
}

static void license()
{
	sout() << otherLicenses << endl;
	// This is a static variable generated by cmake from LICENSE.txt
	sout() << licenseText << endl;
	exit(0);
}

static bool needsHumanTargetedStdout(po::variables_map const& _args)
{
	for (string const& arg: {
		g_argAstJson,
		g_argNatspecUser,
		g_argNatspecDev,
	})
		if (_args.count(arg))
			return true;
	return false;
}

void CommandLineInterface::handleNatspec(bool _natspecDev, string const& _contract)
{
	std::string argName;
	std::string suffix;
	std::string title;

	if (_natspecDev)
	{
		argName = g_argNatspecDev;
		suffix = ".docdev";
		title = "Developer Documentation";
	}
	else
	{
		argName = g_argNatspecUser;
		suffix = ".docuser";
		title = "User Documentation";
	}

	if (m_args.count(argName))
	{
		std::string output = jsonPrettyPrint(
			_natspecDev ?
			m_compiler->natspecDev(_contract) :
			m_compiler->natspecUser(_contract)
		);

		{
			sout() << title << endl;
			sout() << output << endl;
		}

	}
}

bool CommandLineInterface::readInputFilesAndConfigureRemappings()
{
	if (m_args.count(g_argInputFile)) {
		string path = m_args[g_argInputFile].as<string>();
		{
			auto eq = find(path.begin(), path.end(), '=');
			if (eq != path.end())
			{
				if (auto r = CompilerStack::parseRemapping(path))
				{
					m_remappings.emplace_back(std::move(*r));
					path = string(eq + 1, path.end());
				}
				else
				{
					serr() << "Invalid remapping: \"" << path << "\"." << endl;
					return false;
				}
			}
			else
			{
				auto infile = boost::filesystem::path(path);
				if (!boost::filesystem::exists(infile))
				{
					{
						serr() << infile << " is not found." << endl;
						return false;
					}
				}

				if (!boost::filesystem::is_regular_file(infile))
				{
					{
						serr() << infile << " is not a valid file." << endl;
						return false;
					}
				}

				m_sourceCodes[infile.generic_string()] = readFileAsString(infile.string());
				path = boost::filesystem::canonical(infile).string();
			}
			m_allowedDirectories.push_back(boost::filesystem::path(path).remove_filename());
		}
	}
	if (m_sourceCodes.size() == 0)
	{
		serr() << "No input files given. If you wish to use the standard input please specify \"-\" explicitly." << endl;
		return false;
	}

	return true;
}

bool CommandLineInterface::parseArguments(int _argc, char** _argv)
{
	g_hasOutput = false;

	// Declare the supported options.
	po::options_description desc(R"(solc, the Solidity commandline compiler.

This program comes with ABSOLUTELY NO WARRANTY. This is free software, and you
are welcome to redistribute it under certain conditions. See 'solc --license'
for details.

Usage: solc [options] input-file

Example:
solc contract.sol

Allowed options)",
		po::options_description::m_default_line_length,
		po::options_description::m_default_line_length - 23
	);
	desc.add_options()
		(g_argHelp.c_str(), "Show help message and exit.")
		(g_argVersion.c_str(), "Show version and exit.")
		(g_strLicense.c_str(), "Show licensing information and exit.")
		(
			(g_argOutputDir + ",o").c_str(),
			po::value<string>()->value_name("path/to/dir"),
			"Set absolute or relative path for directory for output files."
		)
		(
			(g_argSetContract + ",c").c_str(),
			po::value<string>()->value_name("contractName"),
			"Sets contract name from the source file to be compiled."
		)
		(
			(g_argFile + ",f").c_str(),
			po::value<string>()->value_name("prefixName"),
			"Set prefix of names of output files (*.code and *abi.json)."
		)
		;
	po::options_description outputComponents("Output Components");
	outputComponents.add_options()
		(g_argAstJson.c_str(), "AST of all source files in JSON format.")
		(g_argAstCompactJson.c_str(), "AST of all source files in a compact JSON format.")
		(g_argNatspecUser.c_str(), "Natspec user documentation of all contracts.")
		(g_argNatspecDev.c_str(), "Natspec developer documentation of all contracts.")
		(g_argTvm.c_str(), "Produce TVM assembly (deprecated).")
		(g_argTvmABI.c_str(), "Produce JSON ABI for contract.")
		(g_argTvmPeephole.c_str(), "Run peephole optimization pass")
		(g_argTvmOptimize.c_str(), "Optimize produced TVM assembly code (deprecated)")
		(g_argTvmUnsavedStructs.c_str(), "Enable struct usage analyzer")
        (g_argDebug.c_str(), "Generate debug info");
	desc.add(outputComponents);

	po::options_description allOptions = desc;
	allOptions.add_options()(g_argInputFile.c_str(), po::value<string>(), "input file");

	// All positional options should be interpreted as input files
	po::positional_options_description filesPositions;
	filesPositions.add(g_argInputFile.c_str(), -1);

	// parse the compiler arguments
	try
	{
		po::command_line_parser cmdLineParser(_argc, _argv);
		cmdLineParser.style(po::command_line_style::default_style & (~po::command_line_style::allow_guessing));
		cmdLineParser.options(allOptions).positional(filesPositions);
		po::store(cmdLineParser.run(), m_args);
	}
	catch (po::error const& _exception)
	{
		serr() << _exception.what() << endl;
		return false;
	}

	const bool tvmAbi = m_args.count(g_argTvmABI);
	const bool tvmCode = m_args.count(g_argTvm);
	if (tvmAbi && tvmCode)
	{
		serr() << "Option " << g_argTvmABI << " and " << g_argTvm << " are mutually exclusive." << endl;
		return false;
	}

	if (m_args.count(g_argTvmPeephole)) {
		for (int i = 1; i < _argc; i++) {
			string s = _argv[i];
			if (s != "--" + g_argTvmPeephole) {
				run_peephole_pass(s);
				return false;
			}
		}
		serr() << "Missing filename." << endl;
		return false;
	}

	m_coloredOutput = isatty(STDERR_FILENO);//!m_args.count(g_argNoColor) && (isatty(STDERR_FILENO) || m_args.count(g_argColor));

	if (m_args.count(g_argHelp) || (isatty(fileno(stdin)) && _argc == 1))
	{
		sout() << desc;
		return false;
	}

	if (m_args.count(g_argVersion))
	{
		version();
		return false;
	}

	if (m_args.count(g_strLicense))
	{
		license();
		return false;
	}

	po::notify(m_args);

	return true;
}

bool CommandLineInterface::processInput()
{
	ReadCallback::Callback fileReader = [this](string const& _kind, string const& _path)
	{
		try
		{
			if (_kind != ReadCallback::kindString(ReadCallback::Kind::ReadFile))
				BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment(
					"ReadFile callback used as callback kind " +
					_kind
				));
			auto path = boost::filesystem::path(_path);
			auto canonicalPath = boost::filesystem::weakly_canonical(path);

			if (!boost::filesystem::exists(canonicalPath))
				return ReadCallback::Result{false, "File not found."};

			if (!boost::filesystem::is_regular_file(canonicalPath))
				return ReadCallback::Result{false, "Not a valid file."};

			auto contents = readFileAsString(canonicalPath.string());
			m_sourceCodes[path.generic_string()] = contents;
			return ReadCallback::Result{true, contents};
		}
		catch (Exception const& _exception)
		{
			return ReadCallback::Result{false, "Exception in read callback: " + boost::diagnostic_information(_exception)};
		}
		catch (...)
		{
			return ReadCallback::Result{false, "Unknown exception in read callback."};
		}
	};

	if (!readInputFilesAndConfigureRemappings())
		return false;

	m_compiler = make_unique<CompilerStack>(fileReader);

	unique_ptr<SourceReferenceFormatter> formatter;
	formatter = make_unique<SourceReferenceFormatterHuman>(serr(false), m_coloredOutput);

	try
	{
		if (m_args.count(g_argInputFile))
			m_compiler->setRemappings(m_remappings);
		m_compiler->setSources(m_sourceCodes);

		if (m_args.count(g_argTvmUnsavedStructs))
			m_compiler->setStructWarning(true);

		if (m_args.count(g_argSetContract))
			m_compiler->setMainContract(m_args[g_argSetContract].as<string>());

		if (m_args.count(g_argOutputDir))
			m_compiler->setOutputFolder(m_args[g_argOutputDir].as<string>());

		if (m_args.count(g_argFile))
			m_compiler->setFileNamePrefix(m_args[g_argFile].as<string>());

		if (m_args.count(g_argTvmABI))
			m_compiler->generateAbi();
		if (m_args.count(g_argTvm))
			m_compiler->generateCode();
		if (m_args.count(g_argTvm) == 0 && m_args.count(g_argTvmABI) == 0) {
			m_compiler->generateCode();
			m_compiler->generateAbi();
        }
        m_compiler->withOptimizations();
        if (m_args.count(g_argTvmOptimize))
            serr() << "Flag '--tvm-optimize' is deprecated. Code is optimized by default." << endl;
        if (m_args.count(g_argDebug))
            m_compiler->withDebugInfo();

		string fileName = m_args[g_argInputFile].as<string>();
		m_compiler->setInputFile(fileName);

		bool successful{};
		bool didCompileSomething{};
		std::tie(successful, didCompileSomething) = m_compiler->compile();
		g_hasOutput |= didCompileSomething;

		for (auto const& error: m_compiler->errors())
		{
			g_hasOutput = true;
			formatter->printErrorInformation(*error);
		}

		if (!successful)
		{
			return false;
		}
	}
	catch (CompilerError const& _exception)
	{
		g_hasOutput = true;
		formatter->printExceptionInformation(_exception, "Compiler error");
		return false;
	}
	catch (InternalCompilerError const& _exception)
	{
		serr() <<
			"Internal compiler error during compilation:" <<
			endl <<
			boost::diagnostic_information(_exception);
		return false;
	}
	catch (UnimplementedFeatureError const& _exception)
	{
		serr() <<
			"Unimplemented feature:" <<
			endl <<
			boost::diagnostic_information(_exception);
		return false;
	}
	catch (Error const& _error)
	{
		if (_error.type() == Error::Type::DocstringParsingError)
			serr() << "Documentation parsing error: " << *boost::get_error_info<errinfo_comment>(_error) << endl;
		else
		{
			g_hasOutput = true;
			formatter->printExceptionInformation(_error, _error.typeName());
		}

		return false;
	}
	catch (Exception const& _exception)
	{
		serr() << "Exception during compilation: " << boost::diagnostic_information(_exception) << endl;
		return false;
	}
	catch (std::exception const& _e)
	{
		serr() << "Unknown exception during compilation" << (
			_e.what() ? ": " + string(_e.what()) : "."
		) << endl;
		return false;
	}
	catch (...)
	{
		serr() << "Unknown exception during compilation." << endl;
		return false;
	}

	return true;
}

void CommandLineInterface::handleAst(string const& _argStr)
{
	string title;

	if (_argStr == g_argAstJson)
		title = "JSON AST:";
	else if (_argStr == g_argAstCompactJson)
		title = "JSON AST (compact format):";
	else
		BOOST_THROW_EXCEPTION(InternalCompilerError() << errinfo_comment("Illegal argStr for AST"));

	// do we need AST output?
	if (m_args.count(_argStr))
	{
		vector<ASTNode const*> asts;
		for (auto const& sourceCode: m_sourceCodes)
			asts.push_back(&m_compiler->ast(sourceCode.first));

		bool legacyFormat = !m_args.count(g_argAstCompactJson);

		sout() << title << endl << endl;
		for (auto const& sourceCode: m_sourceCodes)
		{
			sout() << endl << "======= " << sourceCode.first << " =======" << endl;
			ASTJsonConverter(legacyFormat, m_compiler->sourceIndices()).print(sout(), m_compiler->ast(sourceCode.first));
		}
	}
}

bool CommandLineInterface::actOnInput()
{
	outputCompilationResults();
	return !m_error;
}

void CommandLineInterface::outputCompilationResults()
{
	// do we need AST output?
	handleAst(g_argAstJson);
	handleAst(g_argAstCompactJson);

	if (!m_compiler->compilationSuccessful())
	{
		serr() << endl << "Compilation halted after AST generation due to errors." << endl;
		return;
	}

	vector<string> contracts = m_compiler->contractNames();
	for (string const& contract: contracts)
	{
		if (needsHumanTargetedStdout(m_args))
			sout() << endl << "======= " << contract << " =======" << endl;

		handleNatspec(true, contract);
		handleNatspec(false, contract);
	} // end of contracts iteration

	if (!g_hasOutput)
	{
		serr() << "Compiler run successful, no output requested." << endl;
	}
}

}
