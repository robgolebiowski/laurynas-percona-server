/* Copyright (c) 2014, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#ifndef DD__UTILS_INCLUDED
#define DD__UTILS_INCLUDED

#include "my_global.h"

#include <string>
#include <ostream>

namespace dd {

///////////////////////////////////////////////////////////////////////////

class Properties;


/**
  Escaping of a std::string. Escapable characters are '\', '=' and
  ';'. Escape character is '\'. Iterate over all characters of src,
  precede all escapable characters by the escape character and append
  to dst. The source string is not modified.

  @param[out] dst string to which escaped result will be appended.
  @param[in] src source string for escaping
*/
void escape(std::string *dst, const std::string &src);


/**
  In place unescaping of std::string. Escapable characters are '\', '='
  and ';'. Escape character is '\'. Iterate over all characters, remove
  escape character if it precedes an escapable character.

  @param[in,out] dest source and destination string for escaping
  @return             Operation status
    @retval true      if an escapable character is not escaped
    @retval false     if success
*/
bool unescape(std::string &dest);


/**
  Start at it, iterate until we hit an unescaped c or the end
  of the string. The stop character c may be ';' or '='. The
  escape character is '\'. Escapable characters are '\', '='
  and ';'. Hitting an unescaped '=' while searching for ';' is
  an error, and also the opposite. Hitting end of string while
  searching for '=' is an error, but end of string while
  searching for ';' is ok.

  In the event of success, the iterator will be at the
  character to be searched for, or at the end of the string.

  @param[in,out] it  string iterator
  @param         end iterator pointing to string end
  @param         c   character to search for

  @return            Operation status
    @retval true     if an error occurred
    @retval false    if success
*/
bool eat_to(std::string::const_iterator &it,
            std::string::const_iterator end,
            char c);


/**
  Start at it, find first unescaped occorrence of c, create
  destination string and copy substring to destination. Unescape
  the destination string, advance the iterator to get to the
  next character after c, or to end of string.

  In the event of success, the iterator will be at the next
  character after the one that was to be searched for, or at the
  end of the string.

  @param[out]    dest destination string
  @param[in,out] it   string iterator
  @param         end  iterator pointing to string end
  @param         c    character to search for

  @return             Operation status
    @retval true      if an error occurred
    @retval false     if success
*/
bool eat_str(std::string &dest, std::string::const_iterator &it,
             std::string::const_iterator end, char c);


/**
  Start at it, find a key and value separated by an unescaped '='. Value
  is supposed to be terminated by an unescaped ';' or by the end of the
  string. Unescape the key and value and add them to the property
  object. Call recursively to find the remaining pairs.

  @param[in,out] props property object where key and value should be added
  @param[in,out] it    string iterator
  @param         end   iterator pointing to string end

  @return             Operation status
    @retval true      if an error occurred
    @retval false     if success
*/
bool eat_pairs(std::string::const_iterator &it,
               std::string::const_iterator end,
               dd::Properties *props);


///////////////////////////////////////////////////////////////////////////

}

#endif // DD__UTILS_INCLUDED
