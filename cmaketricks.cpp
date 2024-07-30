#include <cassert>
#include <cstdio>
#include <cstring>
#include <regex>
#include <windows.h>
#include <algorithm>

#pragma comment(linker, "/SUBSYSTEM:CONSOLE")

#define CP_UNIXCP 65010  /* Wine extension */
static const auto g_Usage =
    "usage: cmaketricks [option(s)] [file(s)]\n"
    " Utility to remaps CL's Command Files and precompiled headers, as those are not known by the scripts.\n"
    " You can learn more at https://learn.microsoft.com/en-us/cpp/build/reference/cl-command-files\n"
    "\n"
    " Options:\n"
    "  -h  --help   Shows this message\n"
    "      --pch    Remaps the given files as precompiled headers (`cmake_pch.h` and the like)\n"
    "      --cmd    Remaps the given files as command files [DEFAULT BEHAVIOR]\n"
    "      --help   Shows this message\n"
    "  -q  --quiet  Hides errors\n"
    "      --debug  Shows debug output, doesn't replace files, but creates new ones with the `.out` extension\n"
    "\n"
    " Exit codes:\n"
    "  0       Success\n"
    "  1       Generic failure\n"
    "  2       Failed to get address of wine-internal function, did you run the executable under wine?\n"
    "  3       Failed to open file to remap\n"
    "  4       Failed to remap path, wine-internal failure\n";
static const std::regex JUS_PATH{ "\\/.+\\/.+" };                // /absolute/path
static const std::regex ONE_PATH{ "[-\\/][A-Za-z]\\/.*" };       // -I/absolute/path /I/absolute/path
static const std::regex DUO_PATH{ "[-\\/][A-Za-z]{2}\\/.*" };    // -Fo/absolute/path /Fo/absolute/path
static const std::regex TRI_PATH{ "[-\\/][A-Za-z]{3,}:\\/.*" };  // -MANIFESTINPUT:/absolute/path /MANIFESTINPUT:/absolute/path
using GetFilename_t = LPWSTR (*__cdecl)( LPCSTR );
static GetFilename_t g_WineGetDosFilename{ nullptr };
static bool g_Quiet{ false };
static bool g_Debug{ false };
enum class Mode { PCH, CMD };


void remapFile( const char *pFile, Mode pMode );
void remap( char* pPath, int pLen );
const char* token( const char* pBuffer, char* pToken, int pTokenLen );


int main( int argc, char **argv ) {
  if ( argc == 1 ) {
    fputs( "usage: cmaketricks [option(s)] [file(s)]\n", stderr );
    exit( 1 );
  }
  const auto hasArgument = [argc, argv]( const char* pOption ) -> bool {
    return std::find( argv, argv+argc, pOption ) != argv+argc;
  };

  // "parse" arguments
  if ( hasArgument( "--help" ) || hasArgument( "-h" ) ) {
    puts( g_Usage );
    return 0;
  }
  if ( hasArgument( "--quiet" ) || hasArgument( "-q" ) ) {
    g_Quiet = true;
  }
  if ( hasArgument( "--debug" ) ) {
    g_Debug = true;
  }
  Mode mode = hasArgument( "--pch" ) ? Mode::PCH : Mode::CMD;

  // load wine func
  g_WineGetDosFilename = reinterpret_cast<GetFilename_t>( GetProcAddress( GetModuleHandleA( "KERNEL32" ), "wine_get_dos_file_name" ) );
  if ( g_WineGetDosFilename == nullptr ) {
    if (! g_Quiet )
      fputs( "cmdfileremap: cannot get the address of 'wine_get_dos_file_name'\n", stderr );
    exit( 2 );
  }

  // process files
  for ( int i = 1; i < argc; i += 1 ) {
    // skip (unknown) arguments
    if ( argv[i][0] == '-' )
      continue;

    remapFile( argv[i], mode );
  }

  return 0;
}

void remapFile( const char *pFile, Mode pMode ) { // NOLINT(*-no-recursion)
  // open file for reading
  auto file = fopen( pFile, "r" );
  if ( file == nullptr ) {
    if (! g_Quiet )
      fprintf( stdout, "Failed to remap response file `%s`: %s", pFile, strerror( errno ) );
    exit( 3 );
  }

  // find file size
  fseek( file, 0, SEEK_END );
  const auto size = ftell( file );
  fseek( file, 0, SEEK_SET );

  // read the entire file
  const auto buffer = new char[size + 1];
  const auto bufLen = fread( buffer, 1, size, file );
  assert( size == bufLen );
  buffer[size] = '\0';
  // close the file for reading
  fclose( file );

  // open file for writing
  if (! g_Debug )
    file = fopen( pFile, "w" );
  else
    file = fopen( (std::string{pFile} + ".out").c_str(), "w" );
  if ( file == nullptr ) {
    if (! g_Quiet )
      fprintf( stdout, "Failed to remap response file `%s`: %s", pFile, strerror( errno ) );
    exit( 3 );
  }
  // process file
  static constexpr auto TOKEN_SIZE{ 1024 };
  auto* tok = new char[TOKEN_SIZE];
  const char* ptr{ buffer };
  while ( (ptr = token( ptr, tok, TOKEN_SIZE )) ) {
    if ( tok[0] == ' ' ) // empty string
      continue;
    if ( *tok == '\r' || *tok == '\n' ) { // newline
      fputc( *tok, file );
      continue;
    }

    if ( g_Debug )
      printf( "Token: `%s` -> ", tok );
    if ( pMode == Mode::CMD ) {
      if ( std::regex_match( tok, ONE_PATH ) ) {
        remap( tok + 2, TOKEN_SIZE );
        fprintf( file, "\"%s\" ", tok );
      } else if ( std::regex_match( tok, DUO_PATH ) ) {
        remap( tok + 3, TOKEN_SIZE );
        fprintf( file, "\"%s\" ", tok );
        // Forced includes and preprocessed files need extra remapping
        if ( tok[1] == 'F' && (tok[2] == 'I' || tok[2] == 'i') ) {
          remapFile( tok + 3, Mode::PCH );
        }
      } else if ( std::regex_match( tok, TRI_PATH ) ) {
        // the path is after the colon
        remap( strchr( tok, ':' ) + 1, TOKEN_SIZE );
        fprintf( file, "\"%s\" ", tok );
      } else if ( std::regex_match( tok, JUS_PATH ) ) {
        remap( tok, TOKEN_SIZE );
        fprintf( file, "\"%s\" ", tok );
      } else {
        fprintf( file, "\"%s\" ", tok );
      }
    } else if ( pMode == Mode::PCH ) {
      if ( strcmp( tok, "#include" ) == 0 ) {
        // if it is an include, gotta remap!
        fputs( tok, file );
        ptr = token( ptr, tok, TOKEN_SIZE );
        remap( tok, TOKEN_SIZE );
        fprintf( file, " \"%s\"", tok );
      } else {
        // nothing special, just echo it out
        fprintf( file, "%s ", tok );
      }
    }
    if ( g_Debug )
     printf( "`%s`\n", tok );
  }
  // flush and close
  fflush( file );
  fclose( file );
}

void remap( char* pPath, int pLen ) {
  auto winPath = g_WineGetDosFilename( pPath );
  if (! winPath ) {
    if (! g_Quiet )
      fprintf( stderr, "failed to remap path `%s`", pPath );
    exit( 4 );
  }

  WideCharToMultiByte( CP_UNIXCP, 0, winPath, -1, pPath, pLen, nullptr, nullptr );
  HeapFree( GetProcessHeap(), 0, winPath );
}

const char* token( const char* pBuffer, char* pToken, int pTokenLen ) {
  while ( (*pBuffer == ' ' || *pBuffer == '\t') && *pBuffer != '\0' )
    pBuffer += 1;

  if ( *pBuffer == '\r' || *pBuffer == '\n' ) {
    pToken[0] = *pBuffer;
    pToken[1] = '\0';
    return pBuffer + 1;
  }

  if ( *pBuffer == '\0' )
    return nullptr;

  int offset = 0;
  const char* start;
  if ( *pBuffer == '"' ) {  // parse a quoted string
    start = (pBuffer += 1);
    while ( *pBuffer != '"' && *pBuffer != '\0' )
      pBuffer += 1;
  } else {  // parse a single token
    start = pBuffer;
    while ( *pBuffer != ' ' && *pBuffer != '\t' && *pBuffer != '\0' && *pBuffer != '\r' && *pBuffer != '\n' )
      pBuffer += 1;
  }

  // copy the token
  auto size = pBuffer - start;
  assert( size < pTokenLen - 1 );
  memcpy( pToken, start, size );
  pToken[ size ] = '\0';
  // return the new initial position
  return pBuffer + offset;
}
