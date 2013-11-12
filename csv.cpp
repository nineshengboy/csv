#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <errno.h>
#include <regex.h>

#define CSV_TOOL_VERSION "20131112"

// wraps an istream, provide an efficient interface to read lines
class line_reader
{
private:
	std::istream *input;
	bool should_delete_input;
	bool badfile;

	// maximum line length
	unsigned buf_cur;
	unsigned buf_end;
	unsigned buf_size;
	char *buf;

	// memmove buf_cur to buf_start, fill buf_end..buf_size with freshly read data
	void refill_buffer ( )
	{
		if ( buf_cur > 0 )
		{
			if ( buf_end < buf_cur )
				buf_end = buf_cur;

			memmove( buf, buf + buf_cur, buf_end - buf_cur );
			buf_end -= buf_cur;
			buf_cur = 0;
		}

		if ( buf_end < buf_size )
		{
			input->read( buf + buf_end, buf_size - buf_end );
			buf_end += input->gcount();
			if (buf_end > buf_size)
				buf_end = buf_cur;	// just in case
		}
	}

public:
	bool failed_to_open ( ) const
	{
		return badfile;
	}

	// return true if no more data is available from input
	bool eos ( ) const
	{
		if ( input->good() )
			return false;

		if ( buf_cur < buf_end )
			return false;

		return true;
	}

	explicit line_reader ( const char *filename, const unsigned line_max = 64*1024 ) :
		badfile(false),
		buf_cur(0),
		buf_end(0),
		buf_size(line_max)
	{
		buf = new char[buf_size];

		if ( filename && filename[ 0 ] == '-' && filename[ 1 ] == 0 )
		{
			should_delete_input = false;
			input = &std::cin;
		}
		else if ( filename )
		{
			should_delete_input = true;
			input = new std::ifstream(filename);
			if ( ! *input )
			{
				std::cerr << "Cannot open " << filename << ": " << strerror( errno ) << std::endl;
				badfile = true;
				return;
			}
		}
		else if ( isatty( 0 ) )
		{
			std::cerr << "Won't read from <stdin>, is a tty. To force, use '-'." << std::endl;
			badfile = true;
			return;
		}
		else
		{
			should_delete_input = false;
			input = &std::cin;
		}

		input->read( buf, (buf_size > 4096 ? buf_size/16 : buf_size) );
		buf_end = input->gcount();

		// discard utf-8 BOM
		if ( (buf_end >= 3) && buf[0] == '\xef' && buf[1] == '\xbb' && buf[2] == '\xbf' )
			buf_cur += 3;
	}

	~line_reader ( )
	{
		if ( should_delete_input )
		       delete input;

		delete[] buf;
	}

	// read one line from input, starting at buf_cur
	// returns the line start in line_start and the line length in line_length
	// line_length includes the newline character(s)
	// the line is not null-terminated
	// internally, advances buf_cur to the beginning of next line
	// returns false iff line_start = NULL, happens after EOF or on lines > line_max
	// eof is considered as a newline
	// the returned pointer is only valid until the next call to read_line
	bool read_line ( char* *line_start, unsigned *line_length )
	{
		char *nl = NULL;

		if ( buf_cur < buf_end )
			nl = (char*)memchr( (void*)(buf + buf_cur), '\n', buf_end - buf_cur );

		// newline found ?
		if ( nl )
		{
			*line_start = buf + buf_cur;
			*line_length = nl + 1 - *line_start;

			buf_cur += *line_length;
			if (buf_cur > buf_end)
				buf_cur = buf_end;	// just in case

			return true;
		}

		// end of file ?
		if ( !input->good() )
		{
			if ( buf_cur < buf_end )
			{
				*line_start = buf + buf_cur;
				*line_length = buf_end - buf_cur;
				buf_cur = buf_end;

				return true;
			}
			else
			{
				*line_start = NULL;
				*line_length = 0;

				return false;
			}
		}

		// slide existing buffer, read more from input, and retry
		if ( buf_cur > 0 )
		{
			refill_buffer();

			return read_line( line_start, line_length );
		}

		// no newline in whole buffer
		*line_start = NULL;
		*line_length = 0;

		std::string sample( buf, 64 );
		std::cerr << "Line too long, near '" << sample << "'" << std::endl;

		// slide buffer anyway, to avoid infinite loop in badly written clients
		buf_cur = 0;
		buf_end = 0;
		refill_buffer();

		return false;
	}

private:
	line_reader ( const line_reader& );
	line_reader& operator=( const line_reader& );
};

class output_buffer
{
private:
	std::ostream *output;
	bool should_delete_output;
	bool badfile;

	unsigned buf_end;
	unsigned buf_size;
	char *buf;

public:
	bool failed_to_open ( ) const
	{
		return badfile;
	}

	void flush ( )
	{
		if ( buf_end > 0 )
		{
			output->write( buf, buf_end );
			buf_end = 0;
		}

		output->flush();
	}

	void append ( const char *s, const unsigned len )
	{
		unsigned len_left = len;

		while ( len_left >= buf_size - buf_end )
		{
			if ( buf_end < buf_size )
				memcpy( buf + buf_end, s, buf_size - buf_end );
			output->write( buf, buf_size );
			len_left -= buf_size - buf_end;
			s += buf_size - buf_end;
			buf_end = 0;
		}

		if ( len_left > 0 )
		{
			memcpy( buf + buf_end, s, len_left );
			buf_end += len_left;
		}
	}

	void append ( const std::string *str )
	{
		append( str->data(), str->size() );
	}

	void append ( const std::string &str )
	{
		append( &str );
	}

	void append ( const char c )
	{
		if ( buf_end < buf_size - 1 )
			buf[ buf_end++ ] = c;
		else
			append( &c, 1 );
	}

	void append_nl ( )
	{
		append( '\r' );
		append( '\n' );
	}

	explicit output_buffer ( const char *filename, const unsigned buf_size = 64*1024 ) :
		badfile(false),
		buf_end(0),
		buf_size(buf_size)
	{
		buf = new char[buf_size];

		if ( filename )
		{
			should_delete_output = true;
			output = new std::ofstream(filename);
			if ( ! *output )
			{
				std::cerr << "Cannot open " << filename << ": " << strerror( errno ) << std::endl;
				badfile = true;
				return;
			}
		}
		else
		{
			should_delete_output = false;
			output = &std::cout;
		}
	}

	~output_buffer ( )
	{
		flush();

		if ( should_delete_output )
			delete output;

		delete[] buf;
	}

private:
	output_buffer ( const output_buffer& );
	output_buffer& operator=( const output_buffer& );
};

class csv_reader
{
private:
	line_reader *input_lines;
	unsigned line_max;
	char *line_copy;
	bool failed;

	char sep;
	char quot;

	char *cur_line;
	unsigned cur_line_length;
	unsigned cur_line_length_nl;
	unsigned cur_field_offset;

	// set cur_line_length from cur_line_length_nl, trim \r\n
	void trim_newlines ( )
	{
		cur_line_length = cur_line_length_nl;

		if ( cur_line_length > 0 && cur_line[ cur_line_length - 1 ] == '\n' )
			cur_line_length--;

		if ( cur_line_length > 0 && cur_line[ cur_line_length - 1 ] == '\r' )
			cur_line_length--;
	}

public:
	bool failed_to_open ( ) const
	{
		return input_lines->failed_to_open();
	}

	// return true if no more data is available from input_lines
	bool eos ( ) const
	{
		if ( failed )
			return true;

		if ( cur_field_offset <= cur_line_length )
			return false;

		if ( !input_lines->eos() )
			return false;

		return true;
	}

	// reset cur_field_offset to 0, so that subsequent read_csv_field() re-output the current row fields
	void reset_cur_field_offset ( )
	{
		cur_field_offset = 0;
	}

	// line_max is passed to the line_reader, it is also the limit for a full csv row (that may span many lines)
	explicit csv_reader ( const char *filename, const char sep = ',', const char quot = '"', const unsigned line_max = 64*1024 ) :
		line_max(line_max),
		line_copy(NULL),
		failed(false),
		sep(sep),
		quot(quot),
		cur_line(NULL),
		cur_line_length(0),
		cur_line_length_nl(0),
		cur_field_offset(1)
	{
		input_lines = new line_reader(filename, line_max);
	}

	~csv_reader ( )
	{
		if ( line_copy )
			delete[] line_copy;

		delete input_lines;
	}

	// read one line from input_lines
	// invalidates previous read_csv_field pointers
	// return false after EOF
	bool fetch_line ( )
	{
		if ( failed )
			return false;

		if ( input_lines->read_line( &cur_line, &cur_line_length_nl ) )
		{
			cur_field_offset = 0;
			trim_newlines();
			
			return true;
		}

		failed = true;
		cur_field_offset = 1;
		cur_line_length = cur_line_length_nl = 0;

		return false;
	}

	// read one csv field from the current line
	// returns false if no more fields are available in the line, or if there is a syntax error (unterminated quote, end quote followed by neither a quote nor a separator)
	// returns a pointer to the line start, the offset of the current field, and its length
	// the 'field_offset' returned by previous calls for the same line is still valid relative to the new 'line_start' (which may change if one field crosses a line boundary, in that case the lines are copied into an internal buffer)
	bool read_csv_field ( char* *line_start, unsigned *field_offset, unsigned *field_length )
	{
		if ( failed )
			return false;

		if ( cur_field_offset > cur_line_length )
			return false;

		*field_offset = cur_field_offset;
		*line_start = cur_line;

		if ( cur_field_offset == cur_line_length )
		{
			// line ends in a coma
			++cur_field_offset;
			*field_length = 0;

			return true;
		}

		if ( cur_line[ cur_field_offset ] != quot )
		{
			// unquoted field
			char *psep = (char*)memchr( (void*)(cur_line + cur_field_offset), sep, cur_line_length - cur_field_offset );

			if ( psep )
			{
				*field_length = psep - (cur_line + cur_field_offset);
				cur_field_offset += *field_length + 1;
			}
			else
			{
				*field_length = cur_line_length - cur_field_offset;
				cur_field_offset += *field_length + 1;
			}

			return true;
		}

		// quoted field
		*field_length = 1;	// field length until now, including opening quote
		while (1)
		{
			char *pquot = NULL;

			if ( cur_field_offset + *field_length < cur_line_length )
				pquot = (char*)memchr( (void*)(cur_line + cur_field_offset + *field_length), quot,
						cur_line_length - (cur_field_offset + *field_length) );

			if ( pquot )
			{
				// found end quote
				*field_length = pquot - (cur_line + cur_field_offset) + 1;

				if ( cur_field_offset + *field_length < cur_line_length )
				{
					if ( cur_line[ cur_field_offset + *field_length ] == sep )
					{
						// end of field
						cur_field_offset += *field_length + 1;
						return true;
					}

					if ( cur_line[ cur_field_offset + *field_length ] == quot )
					{
						// escaped quote
						++*field_length;
						continue;
					}

					// syntax error
					cur_field_offset += *field_length;
					return false;
				}

				// end quote was at end of line
				cur_field_offset += *field_length + 1;
				return true;
			}

			// no closing quote on current input_lines line
			if ( cur_line != line_copy )
			{
				// copy current line to internal buffer
				if ( !line_copy )
					line_copy = new char[line_max];

				memcpy( line_copy, cur_line, cur_line_length_nl );

				*line_start = cur_line = line_copy;
			}

			char *next_line = NULL;
			unsigned next_line_length_nl = 0;

			input_lines->read_line( &next_line, &next_line_length_nl );

			if ( next_line && cur_line_length_nl + next_line_length_nl <= line_max )
			{
				// next line fits in internal buffer: append
				memcpy( line_copy + cur_line_length_nl, next_line, next_line_length_nl );

				*field_length = cur_line_length_nl - cur_field_offset;

				cur_line_length_nl += next_line_length_nl;

				trim_newlines();
			}
			else
			{
				// reached end of input_lines / line_max with no end quote: return syntax error
				if ( next_line )
				{
					std::string sample( cur_line, 64 );
					std::cerr << "Csv row too long (maybe unclosed quote?) near '" << sample << "'" << std::endl;
				}

				if ( ! input_lines->eos() )
					std::cerr << "Ignoring end of file" << std::endl;

				failed = true;
				cur_field_offset = cur_line_length + 1;
				return false;
			}
		}
	}

	// same as read_csv_field ( line_start, field_offset, field_length ) with simpler args
	// returned values are only valid until the next call to this function (with the 3-args version, it is valid until fetch_line())
	bool read_csv_field ( char* *field_start, unsigned *field_length )
	{
		char *line_start = NULL;
		unsigned field_offset = 0;

		if ( ! read_csv_field( &line_start, &field_offset, field_length ) )
			return false;

		*field_start = line_start + field_offset;

		return true;
	}

	// return an unescaped csv field
	// if unescaped is not NULL, fill it with unescaped data (data appended, string should be empty on call)
	// if unescaped is NULL, and field has no escaped quote, only update field_start and field_length to reflect unescaped data (zero copy)
	// if unescaped is NULL, and field has escaped quotes, allocate a new string and fill it with unescaped data. Caller should free it.
	// returns a pointer to unescaped
	// on return, if unescaped is not NULL, field_start and field_length are undefined.
	std::string* unescape_csv_field ( char* *field_start, unsigned *field_length, std::string* unescaped = NULL ) const
	{
		if ( *field_length <= 0 )
			return unescaped;

		if ( (*field_start)[ 0 ] != quot )
		{
			if ( unescaped )
				unescaped->append( *field_start, *field_length );

			return unescaped;
		}

		++*field_start;
		--*field_length;
		--*field_length;

		while (1)
		{
			char *pquot = NULL;

			if ( *field_length > 0 )
				pquot = (char*)memchr( (void*)*field_start, quot, *field_length );

			if ( pquot )
			{
				// found quote: must be an escape
				if ( ! unescaped )
					unescaped = new std::string;

				// append string start, including 1 quot
				unescaped->append( *field_start, pquot - *field_start + 1 );
				*field_length -= pquot - *field_start + 2;
				*field_start = pquot + 2;
			}
			else
			{
				if ( unescaped )
					unescaped->append( *field_start, *field_length );

				return unescaped;
			}
		}
	}

	// return the escaped version of an unescaped string
	std::string escape_csv_field ( const std::string &str ) const
	{
		std::string ret;

		if ( str.size() == 0 )
			return ret;

		ret.push_back( quot );

		size_t last = 0, next = str.find( quot );
		while ( next != std::string::npos )
		{
			ret.append( str.substr( last, next-last ) );
			ret.push_back( quot );
			ret.push_back( quot );

			last = next + 1;
			next = str.find( quot, last );
		}

		ret.append( str.substr( last ) );
		ret.push_back( quot );

		return ret;
	}

	// parse the current csv line (after a call to fetch_line())
	// return a pointer to a vector of unescaped strings, should be delete by caller
	std::vector<std::string>* parse_line ( )
	{
		std::vector<std::string> *vec = new std::vector<std::string>;
		char *field_start = NULL;
		unsigned field_length = 0;

		while ( read_csv_field( &field_start, &field_length ) )
		{
			std::string unescaped;
			unescape_csv_field( &field_start, &field_length, &unescaped );
			vec->push_back( unescaped );
		}

		return vec;
	}

private:
	csv_reader ( const csv_reader& );
	csv_reader& operator=( const csv_reader& );
};

enum {
	RE_NOCASE,
	RE_INVERT
};

class csv_tool
{
private:
	char sep;
	char quot;
	bool has_headerline;
	unsigned re_flags;

	output_buffer *outbuf;

	csv_reader *reader;
	std::vector<std::string> *headers;
	std::vector<int> *indexes;
	std::vector< std::vector<unsigned>* > *inv_indexes;
	int max_index;

	void cleanup ( )
	{
		if ( reader )
		{
			delete reader;
			reader = NULL;
		}

		if ( headers )
		{
			delete headers;
			headers = NULL;
		}

		if ( indexes )
		{
			delete indexes;
			indexes = NULL;
		}

		if ( inv_indexes )
		{
			for ( unsigned i = 0 ; i < inv_indexes->size() ; ++i )
				if ( (*inv_indexes)[ i ] )
					delete (*inv_indexes)[ i ];

			delete inv_indexes;
			inv_indexes = NULL;
		}
	}

	void count_max_index ( )
	{
		if ( headers )
		{
			max_index = headers->size() - 1;
			return;
		}

		// count columns on the current row
		char *ptr = NULL;
		unsigned len = 0;

		max_index = -1;
		while ( reader->read_csv_field( &ptr, &len ) )
			++max_index;

		// reset internal ptr for subsequent read_csv_fields
		reader->reset_cur_field_offset();
	}

	// return the index of the string str in the string vector headers (case insensitive)
	// checks for numeric indexes if not found (decimal, [0-9]+), <= max_index
	// return -1 if not found
	int parse_index_uint( const std::string &str ) const
	{
		if ( str.size() == 0 )
			return -1;

		if ( headers )
			for ( std::vector<std::string>::const_iterator head_it = headers->begin() ; head_it != headers->end() ; ++head_it )
				if ( ! strcasecmp( str.c_str(), head_it->c_str() ) )
					return head_it - headers->begin();

		int ret = 0;

		for ( unsigned i = 0 ; i < str.size() ; ++i )
		{
			if ( str[i] < '0' || str[i] > '9' )
				return -1;

			ret = ret * 10 + (str[i] - '0');
		}

		if ( ret > max_index )
			return -1;

		return ret;
	}

	// Parse a colspec string (coma-separated list of column names), populate 'indexes'
	//
	// colspec is a coma-separated list of column names, or a coma-separated list of column indexes (numeric, 0-based)
	// colspec may include ranges of columns, with "begin-end". Omit "begin" to start at 0, omit "end" to finish at last column.
	//
	// if a column is not found, its index is set as -1. For ranges, if begin or end is not found, add a single -1 index.
	// without a header list, the current line is parsed and reset to count the fields per row
	void parse_colspec( const std::string &colspec_str )
	{
		indexes = new std::vector<int>;

		// parse colspec_str: split on comas
		std::vector<std::string> colspec_vec;

		if ( colspec_str.size() > 0 )
		{
			size_t off = 0, idx = -1;
			while ( (idx = colspec_str.find( ',', off )) != std::string::npos )
			{
				colspec_vec.push_back( colspec_str.substr( off, idx-off ) );
				off = idx + 1;
			}
			colspec_vec.push_back( colspec_str.substr( off ) );
		}

		// generate indexes, handle ranges in colspec_vec
		for ( std::vector<std::string>::const_iterator spec_it = colspec_vec.begin() ; spec_it != colspec_vec.end() ; ++spec_it )
		{
			int idx = parse_index_uint( *spec_it );

			if ( idx != -1 )
				indexes->push_back( idx );
			else
			{
				// check for ranges
				// handle col names including '-'
				size_t dash_off = -1;

				while (1)
				{
					dash_off = spec_it->find( '-', dash_off + 1 );
					if ( dash_off == std::string::npos )
					{
						std::cerr << "Column not found: " << *spec_it << std::endl;
						indexes->push_back( -1 );
						break;
					}

					int min, max;
					if ( dash_off == 0 )
						min = 0;
					else
						min = parse_index_uint( spec_it->substr( 0, dash_off ) );

					if ( dash_off == spec_it->size() - 1 )
						max = max_index;
					else
						max = parse_index_uint( spec_it->substr( dash_off + 1 ) );

					if ( min != -1 && max != -1 )
					{
						for ( int i = min ; i <= max ; ++i )
							indexes->push_back( i );

						break;
					}
				}
			}
		}

		// inv[ input col idx ] = [ out col idx, out col idx ]
		inv_indexes = new std::vector< std::vector<unsigned>* >( max_index + 1 );
		for ( unsigned idx_out = 0 ; idx_out < indexes->size() ; ++idx_out )
		{
			int idx_in = (*indexes)[ idx_out ];

			if ( idx_in == -1 )
				continue;

			if ( ! (*inv_indexes)[ idx_in ] )
				(*inv_indexes)[ idx_in ] = new std::vector<unsigned>;

			(*inv_indexes)[ idx_in ]->push_back( idx_out );
		}
	}

	// create a csv reader, populate indexes from colspec
	// returns true if everything is fine
	bool start_reader ( const std::string &colspec, const char *filename )
	{
		cleanup();

		reader = new csv_reader( filename, sep, quot );

		if ( reader->failed_to_open() )
		{
			cleanup();
			return false;
		}

		if ( has_headerline )
		{
			if ( ! reader->fetch_line() )
			{
				std::cerr << "Empty file" << std::endl;
				cleanup();
				return false;
			}

			headers = reader->parse_line();
		}

		bool retval = reader->fetch_line();
		count_max_index();

		parse_colspec( colspec );

		return retval;
	}

	// split a string "k1=v1,k2=v2,k3=v3" into vectors [k1, k2, k3] and [v1, v2, v3]
	// k may be omitted with -H
	bool split_colvalspec( const std::string &colval, std::vector<std::string> *cols, std::vector<std::string> *vals )
	{
		size_t off = 0;

		while (1)
		{
			size_t eq_off = colval.find( '=', off );
			if ( eq_off == std::string::npos )
			{
				if ( has_headerline )
				{
					std::cerr << "Invalid colval: no '=' after " << colval.substr( off ) << std::endl;
					return false;
				}

				cols->push_back( colval.substr( 0, 0 ) );
			}
			else
			{
				cols->push_back( colval.substr( off, eq_off - off ) );

				off = eq_off + 1;
			}

			size_t next_off = colval.find( ',', off );
			if ( next_off == std::string::npos )
			{
				vals->push_back( colval.substr( off ) );
				return true;
			}

			vals->push_back( colval.substr( off, next_off - off ) );

			off = next_off + 1;
		}
	}

public:
	explicit csv_tool ( output_buffer *outbuf, char sep = ',', char quot = '"', bool has_headerline = true, unsigned re_flags = 0 ) :
		sep(sep),
		quot(quot),
		has_headerline(has_headerline),
		re_flags(re_flags),
		outbuf(outbuf),
		reader(NULL),
		headers(NULL),
		indexes(NULL),
		inv_indexes(NULL)
	{
	}

	~csv_tool ( )
	{
		cleanup();
	}


	// read one column name (specified in colspec), and dump to outbuf every unescaped row field content for this column
	void extract ( const std::string &colspec, const char *filename )
	{
		if ( ! start_reader( colspec, filename ) )
			return;

		if ( indexes->size() != 1 || (*indexes)[0] < 0 )
		{
			std::cerr << "Invalid colspec" << std::endl;
			return;
		}

		if ( reader->eos() )
			return;

		do
		{
			char *ptr = NULL;
			unsigned len = 0;
			unsigned idx_in = 0;

			while ( reader->read_csv_field( &ptr, &len ) )
			{
				if ( ( idx_in < inv_indexes->size() ) && ( (*inv_indexes)[ idx_in ] ) )
				{
					std::string *str = reader->unescape_csv_field( &ptr, &len );
					if ( str )
						outbuf->append( str );
					else
						outbuf->append( ptr, len );
				}

				// cannot break: a later field may include a newline
				++idx_in;
			}
			outbuf->append_nl();

		} while ( reader->fetch_line() );
	}


	// output a csv containing the columns from colspec of the input csv
	void select ( const std::string &colspec, const char *filename, bool show_headers )
	{
		if ( ! start_reader( colspec, filename ) )
			return;

		if ( show_headers && headers )
		{
			for ( unsigned i = 0 ; i < indexes->size() ; ++i )
			{
				if ( i > 0 )
					outbuf->append( sep );

				int idx_in = (*indexes)[ i ];

				if ( idx_in != -1 )
					outbuf->append( reader->escape_csv_field( (*headers)[ idx_in ] ) );
			}
			outbuf->append_nl();
		}

		if ( reader->eos() )
			return;

		unsigned idx_len = indexes->size();
		unsigned *fld_off = new unsigned[ idx_len ];
		unsigned *fld_len = new unsigned[ idx_len ];

		do
		{
			for ( unsigned idx_out = 0 ; idx_out < idx_len ; ++idx_out )
				fld_off[ idx_out ] = (unsigned)-1;

			// parse input row
			char *line = NULL;
			unsigned f_off = 0, f_len = 0;
			unsigned idx_in = 0;
			while ( reader->read_csv_field( &line, &f_off, &f_len ) )
			{
				if ( idx_in < inv_indexes->size() && (*inv_indexes)[ idx_in ] )
				{
					// current input field appears in output, save its off+len
					for ( unsigned i = 0 ; i < (*inv_indexes)[ idx_in ]->size() ; ++i )
					{
						unsigned idx_out = (*(*inv_indexes)[ idx_in ])[ i ];
						fld_off[ idx_out ] = f_off;
						fld_len[ idx_out ] = f_len;
					}
				}
				++idx_in;
			}

			// generate output row
			for ( unsigned idx_out = 0 ; idx_out < idx_len ; ++idx_out )
			{
				if ( idx_out > 0 )
					outbuf->append( sep );

				if ( fld_off[ idx_out ] != (unsigned)-1 )
					outbuf->append( line + fld_off[ idx_out ], fld_len[ idx_out ] );
			}
			outbuf->append_nl();

		} while ( reader->fetch_line() );


		delete[] fld_off;
		delete[] fld_len;
	}

	// list columns of the file (indexes if -H)
	void listcol ( const char *filename )
	{
		if ( ! start_reader( "", filename ) )
			return;

		if ( headers )
		{
			for ( unsigned i = 0 ; i < headers->size() ; ++i )
			{
				outbuf->append( (*headers)[ i ] );
				outbuf->append_nl();
			}
		}
		else
		{
			char buf[16];
			for ( int i = 0 ; i <= max_index ; ++i )
			{
				unsigned sz = snprintf( buf, sizeof(buf), "%d", i );
				if ( sz < sizeof(buf) )
					outbuf->append( buf, sz );
				outbuf->append_nl();
			}
		}
	}

	// prepend fields to every row (ignore added colnames if -H)
	void addcol ( const std::string &colval, const char *filename )
	{
		std::vector<std::string> cols;
		std::vector<std::string> vals;
		if ( ! split_colvalspec( colval, &cols, &vals ) )
			return;

		if ( ! start_reader( "", filename ) )
			return;

		if ( headers )
		{
			for ( unsigned i = 0 ; i < cols.size() ; ++i )
			{
				outbuf->append( reader->escape_csv_field( cols[i] ) );
				outbuf->append( sep );
			}

			for ( unsigned i = 0 ; i < headers->size() ; ++i )
			{
				outbuf->append( reader->escape_csv_field( (*headers)[i] ) );

				if ( i + 1 < headers->size() )
					outbuf->append( sep );
			}

			outbuf->append_nl();
		}

		if ( reader->eos() )
			return;

		do
		{
			for ( unsigned i = 0 ; i < vals.size() ; ++i )
			{
				if ( i > 0 )
					outbuf->append( sep );
				outbuf->append( vals[ i ] );
			}

			char *fld = NULL;
			unsigned fld_len = 0;

			while ( reader->read_csv_field( &fld, &fld_len, &fld_len ) )
			{
				outbuf->append( sep );
				outbuf->append( fld, fld_len );
			}

			outbuf->append_nl();

		} while ( reader->fetch_line() );
	}

	// filter csv, display only lines whose field value match a regexp
	void grepcol ( const std::string &colval, const char *filename )
	{
		std::vector<std::string> cols;
		std::vector<std::string> vals;
		if ( ! split_colvalspec( colval, &cols, &vals ) )
			return;

		// merge cols in a colspec
		std::string colspec;
		for ( unsigned i = 0 ; i < cols.size() ; ++i )
		{
			if ( i > 0 )
				colspec.push_back( ',' );

			colspec.append( cols[ i ] );
		}

		regex_t *vals_re = new regex_t[ vals.size() ];

		int flags = REG_NOSUB | REG_EXTENDED;
		if ( re_flags & ( 1 << RE_NOCASE ) )
			flags |= REG_ICASE;

		for ( unsigned i = 0 ; i < vals.size() ; ++i )
		{

			int err = regcomp( &vals_re[ i ], vals[ i ].c_str(), flags );
			if ( err )
			{
				char errbuf[1024];
				regerror( err, &vals_re[ i ], errbuf, sizeof(errbuf) );
				std::cerr << "Invalid regexp /" << vals[ i ] << "/ : " << errbuf << std::endl;

				for ( unsigned j = 0 ; j < i ; ++j )
					regfree( &vals_re[ j ] );
				delete[] vals_re;

				return;
			}
		}

		if ( ! start_reader( colspec, filename ) )
		{
			for ( unsigned i = 0 ; i < vals.size() ; ++i )
				regfree( &vals_re[ i ] );
			delete[] vals_re;

			return;
		}

		if ( headers )
		{
			for ( unsigned i = 0 ; i < headers->size() ; ++i )
			{
				if ( i > 0 )
					outbuf->append( sep );

				outbuf->append( reader->escape_csv_field( (*headers)[i] ) );
			}

			outbuf->append_nl();
		}

		if ( reader->eos() )
		{
			for ( unsigned i = 0 ; i < vals.size() ; ++i )
				regfree( &vals_re[ i ] );
			delete[] vals_re;

			return;
		}

		const unsigned stats_batch_size = 16*1024;
		unsigned stats_seen = 0;
		unsigned stats_match = (headers ? 1 : 0);
		bool invert = re_flags & ( 1 << RE_INVERT );
		do
		{
			char *line = NULL;
			unsigned f_off = 0, f_len = 0;
			unsigned idx_in = 0;
			bool show = false;

			while ( reader->read_csv_field( &line, &f_off, &f_len ) )
			{
				if ( ( idx_in < inv_indexes->size() ) && ( (*inv_indexes)[ idx_in ] ) )
				{
					std::string str;
					char *ptr = line + f_off;
					reader->unescape_csv_field( &ptr, &f_len, &str );

					for ( unsigned i = 0 ; i < (*inv_indexes)[ idx_in ]->size() ; ++i )
					{
						unsigned idx_g = (*(*inv_indexes)[ idx_in ])[ i ];
						if ( idx_g < vals.size() && regexec( &vals_re[ idx_g ], str.c_str(), 0, NULL, 0 ) != REG_NOMATCH )
							show = true;
					}
				}
				++idx_in;
			}

			if ( show ^ invert )
			{
				outbuf->append( line, f_off + f_len );
				outbuf->append_nl();
				++stats_match;
			}
			++stats_seen;

			// manually flush if output is sparse
			if ( stats_seen > stats_batch_size )
			{
				if ( ( stats_match > 0 ) && ( stats_match < stats_batch_size / 8 ) )
					outbuf->flush();
				stats_seen = 0;
				// ensure we'll flush next time if we didnt now and next is empty
				stats_match = ( ( stats_match >= stats_batch_size / 8 ) ? 1 : 0 );
			}

		} while ( reader->fetch_line() );

		for ( unsigned i = 0 ; i < vals.size() ; ++i )
			regfree( &vals_re[ i ] );
		delete[] vals_re;
	}

	// dump csv rows, prefix each field with its colname
	void inspect ( const char *filename )
	{
		if ( ! start_reader( "", filename ) )
			return;

		if ( reader->eos() )
			return;

		unsigned long lineno = 0;
		char linebuf[16];
		do
		{
			{
				unsigned linebuf_sz = snprintf( linebuf, sizeof(linebuf), "%03lu:", lineno++ );
				if ( linebuf_sz > sizeof(linebuf) )
					linebuf_sz = sizeof(linebuf);
				std::string linestr(linebuf, linebuf_sz);
				outbuf->append( linestr );
			}

			// parse input row
			char *fld = NULL;
			unsigned f_len = 0;
			unsigned colnum = 0;
			while ( reader->read_csv_field( &fld, &f_len ) )
			{
				// create & populate headers ondemand
				if ( ! headers )
					headers = new std::vector<std::string>;

				if ( colnum >= headers->size() )
				{
					char fldbuf[16];
					unsigned fldbuf_sz = snprintf( fldbuf, sizeof(fldbuf), "%u", colnum );
					if ( fldbuf_sz > sizeof(fldbuf) )
						fldbuf_sz = sizeof(fldbuf);
					std::string fldstr(fldbuf, fldbuf_sz);
					headers->push_back( fldstr );
				}

				if ( colnum > 0 )
					outbuf->append( sep );

				outbuf->append( headers->at( colnum ) );
				outbuf->append( '=' );
				outbuf->append( fld, f_len );
				++colnum;
			}
			outbuf->append_nl();

		} while ( reader->fetch_line() );
	}

};

static const char *usage =
"Usage: csv [options] <mode>\n"
" Options:\n"
"          -V                 display version information and exit\n"
"          -h                 display help (this text) and exit\n"
"          -o <outfile>       specify output file (default=stdout)\n"
"          -s <separator>     csv field separator (default=',')\n"
"          -q <quote>         csv quote character (default='\"')\n"
"          -H                 csv files have no header line\n"
"                             columns are specified as number (first col is 0)\n"
"          -i                 case insensitive regex (grep mode)\n"
"          -v                 invert regex: show non-matching lines (grep mode)\n"
"\n"
"csv addcol <col1>=<val1>,..  prepend an column to the csv with fixed value\n"
"csv extract <column>         extract one column data\n"
"csv grepcol <col1>=<val1>,.. create a csv with only the lines where colX has value X (regexp)\n"
"                             with multiple colval, show line if any one match (c1=~v1 OR c2=~v2)\n"
"csv select <col1>,<col2>,..  create a new csv with a subset/reordered columns\n"
"csv listcol                  list csv column names, one per line\n"
"csv inspect                  dump csv file, prefix each field with its column name\n"
;

static const char *version_info =
"CSV tool version " CSV_TOOL_VERSION "\n"
"Copyright (c) 2013 Yoann Guillot\n"
"Licensed under the WtfPLv2, see http://www.wtfpl.net/\n"
;

int main ( int argc, char * argv[] )
{
	int opt;
	char *outfile = NULL;
	char sep = ',';
	char quot = '"';
	unsigned re_flags = 0;
	bool has_headerline = true;


	while ( (opt = getopt(argc, argv, "hVo:s:q:Hiv")) != -1 )
	{
		switch (opt)
		{
		case 'h':
			std::cout << usage << std::endl;
			return EXIT_SUCCESS;

		case 'V':
			std::cout << version_info << std::endl;
			return EXIT_SUCCESS;

		case 'o':
			outfile = optarg;
			break;

		case 's':
			sep = *optarg;
			break;

		case 'q':
			quot = *optarg;
			break;

		case 'H':
			has_headerline = false;
			break;

		case 'i':
			re_flags |= 1 << RE_NOCASE;
			break;

		case 'v':
			re_flags |= 1 << RE_INVERT;
			break;

		default:
			std::cerr << "Unknwon option: " << opt << std::endl << usage << std::endl;
			return EXIT_FAILURE;
		}
	}

	if ( optind >= argc )
	{
		std::cerr << "No mode specified" << std::endl << usage << std::endl;
		return EXIT_FAILURE;
	}

	output_buffer outbuf( outfile );
	if ( outbuf.failed_to_open() )
		return EXIT_FAILURE;

	csv_tool csv( &outbuf, sep, quot, has_headerline, re_flags );

	std::string mode = argv[optind++];

	if ( mode == "extract" || mode == "e" || mode == "x" )
	{
		if ( optind >= argc )
		{
			std::cerr << "No column specified" << std::endl << usage << std::endl;
			return EXIT_FAILURE;
		}
		std::string colspec = argv[ optind++ ];

		if ( optind >= argc )
			csv.extract( colspec, NULL );
		else
			for ( int i = optind ; i < argc ; ++i )
				csv.extract( colspec, argv[ i ] );
	}
	else if ( mode == "select" || mode == "map" || mode == "s" || mode == "m" )
	{
		if ( optind >= argc )
		{
			std::cerr << "No column specified" << std::endl << usage << std::endl;
			return EXIT_FAILURE;
		}
		std::string colspec = argv[ optind++ ];

		if ( optind >= argc )
			csv.select( colspec, NULL, true );
		else
		{
			for ( int i = optind ; i < argc ; ++i )
				csv.select( colspec, argv[ i ], (i == optind) );
		}
	}
	else if ( mode == "listcol" || mode == "l" )
	{
		if ( optind >= argc )
			csv.listcol( NULL );
		else
		{
			for ( int i = optind ; i < argc ; ++i )
				csv.listcol( argv[ i ] );
		}
	}
	else if ( mode == "addcol" || mode == "a" )
	{
		if ( optind >= argc )
		{
			std::cerr << "No colval specified" << std::endl << usage << std::endl;
			return EXIT_FAILURE;
		}
		std::string colval = argv[ optind++ ];

		if ( optind >= argc )
			csv.addcol( colval, NULL );
		else
		{
			for ( int i = optind ; i < argc ; ++i )
				csv.addcol( colval, argv[ i ] );
		}
	}
	else if ( mode == "grepcol" || mode == "grep" || mode == "g" )
	{
		if ( optind >= argc )
		{
			std::cerr << "No colval specified" << std::endl << usage << std::endl;
			return EXIT_FAILURE;
		}
		std::string colval = argv[ optind++ ];

		if ( optind >= argc )
			csv.grepcol( colval, NULL );
		else
		{
			for ( int i = optind ; i < argc ; ++i )
				csv.grepcol( colval, argv[ i ] );
		}
	}
	else if ( mode == "inspect" || mode == "i" )
	{
		if ( optind >= argc )
			csv.inspect( NULL );
		else
		{
			for ( int i = optind ; i < argc ; ++i )
				csv.inspect( argv[ i ] );
		}
	}
	else
	{
		std::cerr << "Unsupported mode " << mode << std::endl << usage << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
