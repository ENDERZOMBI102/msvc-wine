#include <cassert>
#include <cstdio>
#include <cstring>
#include <regex>
#include <windows.h>

#define CP_UNIXCP 65010  /* Wine extension */
static const auto g_Usage =
    "usage: cmdfileremap [option(s)] [file(s)]\n"
    " Remaps Command Files indented to be used by cl, as those are not known by the scripts.\n"
    " You can learn more at https://learn.microsoft.com/en-us/cpp/build/reference/cl-command-files\n"
    "\n"
    " Options:\n"
    "      --help   Shows this message\n"
    "  -q  --quiet  Hides output\n"
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


void remapFile( const char *pFile );
void remap( char* pPath, int pLen );
const char* token( const char* pBuffer, char* pToken, int pTokenLen );


int main( int argc, char **argv ) {
  if ( argc == 1 ) {
    std::fputs( "usage: cmdfileremap [option(s)] [file(s)]\n", stderr );
    std::exit( 1 );
  }
  if ( std::strcmp( argv[1], "--help" ) == 0 ) {
    std::puts( g_Usage );
    return 0;
  }
  if ( std::strcmp( argv[1], "--quiet" ) == 0 || std::strcmp( argv[1], "-q" ) == 0 ) {
    g_Quiet = true;
  }

  g_WineGetDosFilename = reinterpret_cast<GetFilename_t>( GetProcAddress( GetModuleHandleA( "KERNEL32" ), "wine_get_dos_file_name" ) );
  if (g_WineGetDosFilename == nullptr ) {
    if (! g_Quiet )
      std::fputs( "cmdfileremap: cannot get the address of 'wine_get_dos_file_name'\n", stderr );
    std::exit( 2 );
  }

  for ( int i = 1; i < argc; i += 1 ) {
    // skip (unknown) arguments
    if ( argv[i][0] == '-' )
      continue;

    remapFile( argv[i] );
  }

  return 0;
}

void remapFile( const char *pFile ) {
  // open file for reading
  auto file = std::fopen( pFile, "r" );
  if ( file == nullptr ) {
    if (! g_Quiet )
      std::fprintf( stdout, "Failed to remap response file `%s`: %s", pFile, std::strerror( errno ) );
    std::exit( 3 );
  }

  // find file size
  std::fseek( file, 0, SEEK_END );
  const auto size = std::ftell( file );
  std::fseek( file, 0, SEEK_SET );

  // read the entire file
  const auto buffer = new char[size + 1];
  const auto bufLen = std::fread( buffer, 1, size, file );
  assert( size == bufLen );
  buffer[size] = '\0';
  // close the file for reading
  std::fclose( file );

  // open file for writing
  file = std::fopen( pFile, "w" );
  if ( file == nullptr ) {
    if (! g_Quiet )
      std::fprintf( stdout, "Failed to remap response file `%s`: %s", pFile, strerror( errno ) );
    std::exit( 3 );
  }
  // process file
  auto* tok = new char[1024];
  for ( const char* ptr = token( buffer, tok, 1024 ); ptr; ptr = token( ptr, tok, 1024 ) ) {
    if ( std::regex_match( tok, ONE_PATH ) ) {
      remap( tok + 2, 1024 );
      std::fprintf( file, "\"%2s%s\" ", tok, tok );
    } else if ( std::regex_match( tok, DUO_PATH ) ) {
      remap( tok + 3, 1024 );
      std::fprintf( file, "\"%3s%s\" ", tok, tok );
    } else if ( std::regex_match( tok, TRI_PATH ) ) {
      // save arg
      auto len = tok - std::strchr( tok, ':' );
      auto x = new char[len + 1];
      strncpy( x, tok, len - 1 );
      x[len] = '\0';
      // remap and print
      remap( tok + len, 1024 );
      std::fprintf( file, "\"%s%s\" ", x, tok );
    } else if ( std::regex_match( tok, JUS_PATH ) ) {
      remap( tok, 1024 );
      std::fprintf( file, "\"%s\" ", tok );
    } else {
      std::fprintf( file, "\"%s\" ", tok );
    }
  }
  // flush and close
  std::fflush( file );
  std::fclose( file );
}

void remap( char* pPath, int pLen ) {
  auto winPath = g_WineGetDosFilename( pPath );
  if (! winPath ) {
    if (! g_Quiet )
      std::fprintf( stderr, "failed to remap path `%s`", pPath );
    std::exit( 4 );
  }

  WideCharToMultiByte( CP_UNIXCP, 0, winPath, -1, pPath, pLen, nullptr, nullptr );
  HeapFree( GetProcessHeap(), 0, winPath );
}

const char* token( const char* pBuffer, char* pToken, int pTokenLen ) {
  while ( ( *pBuffer == ' ' || *pBuffer == '\t' ) && *pBuffer != '\0' )
    pBuffer += 1;

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
    while ( *pBuffer != ' ' && *pBuffer != '\t' && *pBuffer != '\0' )
      pBuffer += 1;
  }

  // copy the token
  auto size = pBuffer - start;
  assert( size < pTokenLen - 1 );
  std::memcpy( pToken, start, size );
  pToken[ size ] = '\0';
  // return the new initial position
  return pBuffer + offset;
}
