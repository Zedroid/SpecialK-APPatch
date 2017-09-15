/**
 * This file is part of Special K.
 *
 * Special K is free software : you can redistribute it
 * and/or modify it under the terms of the GNU General Public License
 * as published by The Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * Special K is distributed in the hope that it will be useful,
 *
 * But WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Special K.
 *
 *   If not, see <http://www.gnu.org/licenses/>.
 *
**/

#include <windows.h>
#include <string>
#include <sys/stat.h>

#include <SpecialK/ini.h>
#include <SpecialK/log.h>
#include <SpecialK/utility.h>

std::wstring
ErrorMessage (errno_t        err,
              const char*    args,
              const wchar_t* ini_name,
              UINT           line_no,
              const char*    function_name,
              const char*    file_name)
{
  wchar_t wszFormattedError [1024];

  *wszFormattedError = L'\0';

  swprintf ( wszFormattedError, 1024,
             L"\n"
             L"Line %u of %hs (in %hs (...)):\n"
             L"------------------------\n\n"
             L"%hs\n\n  File: %s\n\n"
             L"\t>> %s <<",
               line_no,
                 file_name,
                   function_name,
                     args,
                       ini_name,
                         _wcserror (err) );


  return wszFormattedError;
}

#define TRY_FILE_IO(x,y,z) { (z) = ##x; if ((z) == nullptr) \
dll_log.Log (L"[ SpecialK ] %ws", ErrorMessage (GetLastError (), #x, (y), __LINE__, __FUNCTION__, __FILE__).c_str ()); }

iSK_INI::iSK_INI (const wchar_t* filename)
{
  if (wcsstr (filename, L"Version"))
    SK_CreateDirectories (filename);

  sections.clear ();

  // We skip a few bytes (Unicode BOM) in crertain cirumstances, so this is the
  //   actual pointer we need to free...
  wchar_t* alloc = nullptr;

  wszName =
    new wchar_t [wcslen (filename) + 2] { };

  wcscpy (wszName, filename);

  TRY_FILE_IO (_wfsopen (filename, L"rb", _SH_DENYNO), filename, fINI);

  if (fINI != nullptr)
  {
    auto size =
      static_cast <long> (SK_GetFileSize (filename));

    wszData = new wchar_t [size + 2] { };
    alloc   = wszData;

    fread (wszData, size, 1, fINI);

    // First, consider Unicode
    // UTF16-LE  (All is well in the world)
    if (*wszData == 0xFEFF)
    {
      ++wszData; // Skip the BOM

      encoding_ = INI_UTF16LE;
    }

    // UTF16-BE  (Somehow we are swapped)
    else if (*wszData == 0xFFFE)
    {
      dll_log.Log ( L"[INI Parser] Encountered Byte-Swapped Unicode INI "
                    L"file ('%s'), attempting to recover...",
                      wszName );

      wchar_t* wszSwapMe = wszData;

      for (int i = 0; i < size; i += 2)
      {
        *wszSwapMe++ =
           _byteswap_ushort (*wszSwapMe);
      }

      ++wszData; // Skip the BOM

      encoding_ = INI_UTF16BE;
    }

    // Something else, if it's ANSI or UTF-8, let's hope Windows can figure
    //   out what to do...
    else
    {
      // Skip the silly UTF8 BOM if it is present
      bool utf8 = (reinterpret_cast <unsigned char *> (wszData)) [0] == 0xEF &&
                  (reinterpret_cast <unsigned char *> (wszData)) [1] == 0xBB &&
                  (reinterpret_cast <unsigned char *> (wszData)) [2] == 0xBF;

      const uintptr_t offset =
        utf8 ? 3 : 0;

      const int       real_size =
        size - static_cast <int> (offset);

      char*           start_addr =
      (reinterpret_cast <char *> (wszData)) + offset;

      auto*           string =
        new char [real_size];

      memcpy (string, start_addr, real_size);

      delete [] wszData;

      int converted_size =
        MultiByteToWideChar ( CP_UTF8, 0, string, real_size, nullptr, 0 );

      if (! converted_size)
      {
        dll_log.Log ( L"[INI Parser] Could not convert UTF-8 / ANSI Encoded "
                      L".ini file ('%s') to UTF-16, aborting!",
                        wszName );
        wszData = nullptr;

        fclose (fINI);

        delete [] string;

        return;
      }

      wszData =
        new wchar_t [converted_size + 2] { };

      MultiByteToWideChar ( CP_UTF8, 0, string, real_size, wszData, converted_size );

      //dll_log.Log ( L"[INI Parser] Converted UTF-8 INI File: '%s'",
                      //wszName );

      delete [] string;

      // No Byte-Order Marker
      alloc = wszData;

      encoding_ = INI_UTF8;
    }

    parse ();

    // TODO:  Should we keep data in unparsed format?
//    delete [] alloc;
//    wszData = nullptr;
    // Why not? It would make re-parsing safer

    fflush (fINI);
    fclose (fINI);
  }

  else
  {
    wszData = nullptr;
  }
}

iSK_INI::~iSK_INI (void)
{
  if (Release () == 0)
  {
    if (wszName != nullptr)
    {
      delete [] wszName;
      wszName = nullptr;
    }

    if (wszData != nullptr)
    {
      delete[] wszData;
      wszData = nullptr;
    }
  }
}

auto wcrlen =
  [](wchar_t *_start, wchar_t *_end) ->
    size_t
    {
      size_t _len = 0;

      wchar_t* _it = _start;
      while   (_it < _end)
      {
          _it = CharNextW (_it);
        ++_len;
      }

      return _len;
    };

iSK_INISection
Process_Section (wchar_t* name, wchar_t* start, wchar_t* end)
{
  iSK_INISection section (name);

  const wchar_t* penultimate = CharPrevW (start, end);
        wchar_t* key         = start;

  for (wchar_t* k = key; k < end; k = CharNextW (k))
  {
    if (k < penultimate && *k == L'=')
    {
      auto*    key_str = new wchar_t [k - key + 2] { };
      size_t   key_len =          wcrlen (key, k);
      wcsncpy (key_str,                   key, key_len);

      wchar_t* value =
        CharNextW (k);

      for (wchar_t* l = value; l <= end; l = CharNextW (l))
      {
        if (l > penultimate || *l == L'\n')
        {
          key = CharNextW (l);
            k = key;

          if (l == end)
          {
            l = CharNextW (l);
            k = end;
          }

          auto*    val_str = new wchar_t [l - value + 2] { };
          size_t   val_len = wcrlen          (value, l);
          wcsncpy (val_str,                   value, val_len);

          section.add_key_value (key_str, val_str);

          delete [] val_str;

          l = end + 1;
        }
      }

      delete [] key_str;
    }
  }

  return section;
}

bool
Import_Section (iSK_INISection& section, wchar_t* start, wchar_t* end)
{
  const wchar_t* penultimate = CharPrevW (start, end);
        wchar_t* key         = start;

  for (wchar_t* k = key; k < end; k = CharNextW (k))
  {
    if (k < penultimate && *k == L'=')
    {
      auto*    key_str = new wchar_t [k - key + 2] { };
      size_t   key_len =          wcrlen (key, k);
      wcsncpy (key_str,                   key, key_len);

      wchar_t* value =
        CharNextW (k);

      for (wchar_t* l = value; l <= end; l = CharNextW (l))
      {
        if (l > penultimate || *l == L'\n')
        {
          key = CharNextW (l);
            k = key;

          auto*    val_str = new wchar_t [l - value + 2] { };
          size_t   val_len = wcrlen          (value, l);
          wcsncat (val_str,                   value, val_len);

          // Prefer to change an existing value
          if (section.contains_key (key_str))
          {
            std::wstring& val =
              section.get_value (key_str);

            val = val_str;
          }

          // But create a new one if it doesn't already exist
          else {
            section.add_key_value (key_str, val_str);
          }

          delete [] val_str;

          l = end;
        }
      }

      delete [] key_str;
    }
  }

  return true;
}

void
__stdcall
iSK_INI::parse (void)
{
  if (wszData != nullptr)
  {
    int len =
      lstrlenW (wszData);

    // We don't want CrLf, just Lf
    bool strip_cr = false;

    wchar_t* wszStrip =
      &wszData [0];

    // Find if the file has any Cr's
    for (int i = 0; i < len; i++)
    {
      if (*wszStrip == L'\r')
      {
        strip_cr = true;
        break;
      }

      wszStrip = CharNextW (wszStrip);
    }

    wchar_t* wszDataEnd =
      &wszData [0];

    if (strip_cr)
    {
      wchar_t* wszDataNext =
        &wszData [0];

      for (int i = 0; i < len; i++)
      {
        if (*wszDataNext != L'\r')
        {
          *wszDataEnd = *wszDataNext;
           wszDataEnd = CharNextW (wszDataEnd);
        }

        wszDataNext = CharNextW (wszDataNext);
      }

      const wchar_t* wszNext =
        CharNextW (wszDataNext);

      memset ( wszDataEnd,
                 0x00,
                   reinterpret_cast <uintptr_t> (wszNext) -
                   reinterpret_cast <uintptr_t> (wszDataEnd) );

      len =
        lstrlenW (wszData);
    }

    else
    {
      for (int i = 0; i < len; i++)
      {
        wszDataEnd = CharNextW (wszDataEnd);
      }
    }

    wchar_t* wszSecondToLast =
      CharPrevW (wszData, wszDataEnd);

    wchar_t* begin           = nullptr;
    wchar_t* end             = nullptr;
    wchar_t* wszDataCur      = &wszData [0];

    for (wchar_t* i = wszDataCur; i < wszDataEnd && i != nullptr; i = CharNextW (i))
    {
      if (*i == L'[' && (i == wszData || *CharPrevW (&wszData [0], i) == L'\n'))
      {
        begin = CharNextW (i);
      }

      if (*i == L']' && (i == wszSecondToLast || *CharNextW (i) == L'\n'))
        end = i;

      if (begin != nullptr && end != nullptr && begin < end)
      {
        auto*    sec_name = new wchar_t    [end - begin + 2] { };
        size_t   sec_len  = wcrlen  (begin, end);
        wcsncpy (sec_name,           begin, sec_len);

        wchar_t* start  = CharNextW (CharNextW (end));
        wchar_t* finish = start;

        bool eof = false;
        for (wchar_t* j = start; j <= wszDataEnd; j = CharNextW (j))
        {
          if (j == wszDataEnd)
          {
            finish = j;
            eof    = true;
            break;
          }

          wchar_t *wszPrev = nullptr;

          if (*j == L'[' && (*(wszPrev = CharPrevW (start, j)) == L'\n'))
          {
            finish = wszPrev;
            break;
          }
        }

        iSK_INISection section =
          Process_Section (sec_name, start, finish);

        sections.emplace (
          std::make_pair (sec_name, section)
        );

        ordered_sections.emplace_back (sec_name);

        delete [] sec_name;

        if (eof)
          break;

        i = finish;

        end   = nullptr;
        begin = nullptr;
      }
    }
  }
}

void
__stdcall
iSK_INI::import (const wchar_t* import_data)
{
  wchar_t* wszImport = _wcsdup (import_data);

  if (wszImport != nullptr)
  {
    int len = lstrlenW (wszImport);

    // We don't want CrLf, just Lf
    bool strip_cr = false;

    wchar_t* wszStrip =
      &wszImport [0];

    // Find if the file has any Cr's
    for (int i = 0; i < len; i++)
    {
      if (*wszStrip == L'\r')
      {
        strip_cr = true;
        break;
      }

      wszStrip = CharNextW (wszStrip);
    }

    wchar_t* wszImportEnd =
      &wszImport [0];

    if (strip_cr)
    {
      wchar_t* wszImportNext =
        &wszImport [0];

      for (int i = 0; i < len; i++)
      {
        if (*wszImportNext != L'\r')
        {
          *wszImportEnd = *wszImportNext;
           wszImportEnd = CharNextW (wszImportEnd);
        }

        wszImportNext   = CharNextW (wszImportNext);
      }

      const wchar_t* wszNext =
        CharNextW (wszImportNext);

      memset ( wszImportEnd,
                 0x00,
                   reinterpret_cast <uintptr_t> (wszNext) -
                   reinterpret_cast <uintptr_t> (wszImportEnd) );

      len =
        lstrlenW (wszImport);
    }

    else
    {
      for (int i = 0; i < (len - 1); i++)
      {
        wszImportEnd = CharNextW (wszImportEnd);
      }
    }

    wchar_t* wszSecondToLast =
      CharPrevW (wszImport, wszImportEnd);

    wchar_t* begin           = nullptr;
    wchar_t* end             = nullptr;

    wchar_t* wszImportCur    = &wszImport [0];

    for (wchar_t* i = wszImportCur; i < wszImportEnd && i != nullptr; i = CharNextW (i))
    {
      if (*i == L'[' && (i == wszImport || *CharPrevW (&wszImport [0], i) == L'\n'))
      {
        begin = CharNextW (i);
      }

      if (*i == L']' && (i == wszSecondToLast || *CharNextW (i) == L'\n'))
        end = i;

      if (begin != nullptr && end != nullptr)
      {
        auto*    sec_name = new wchar_t [end - begin + 2] { };
        size_t   sec_len  = wcrlen            (begin, end);
        wcsncpy (sec_name,                     begin, sec_len);

        //MessageBoxW (NULL, sec_name, L"Section", MB_OK);

        wchar_t* start  = CharNextW (CharNextW (end));
        wchar_t* finish = start;
        bool     eof    = false;

        for (wchar_t* j = start; j <= wszImportEnd; j = CharNextW (j))
        {
          if (j == wszImportEnd)
          {
            finish = j;
            eof    = true;
            break;
          }

          wchar_t *wszPrev = nullptr;

          if (*j == L'[' && (*(wszPrev = CharPrevW (start, j)) == L'\n'))
          {
            finish = wszPrev;
            break;
          }
        }

        // Import if the section already exists
        if (contains_section (sec_name))
        {
          iSK_INISection& section =
            get_section  (sec_name);

          Import_Section (section, start, finish);
        }

        // Insert otherwise
        else
        {
          iSK_INISection section =
            Process_Section (sec_name, start, finish);

          sections.emplace  (
            std::make_pair  (sec_name, section)
          );

          ordered_sections.emplace_back (sec_name);
        }

        delete [] sec_name;

        if (eof)
          break;

        i = finish;

        end   = nullptr;
        begin = nullptr;
      }
    }
  }

  delete [] wszImport;
}

std::wstring invalid = L"Invalid";

std::wstring&
__stdcall
iSK_INISection::get_value (const wchar_t* key)
{
  auto it_key =
    keys.find (key);

  if (it_key != keys.end ())
    return (*it_key).second;

  return invalid;
}

void
__stdcall
iSK_INISection::set_name (const wchar_t* name_)
{
  name = name_;
}

bool
__stdcall
iSK_INISection::contains_key (const wchar_t* key)
{
  return keys.count (key) > 0;
}

void
__stdcall
iSK_INISection::add_key_value (const wchar_t* key, const wchar_t* value)
{
  keys.emplace              ( std::make_pair (key, value) );
  ordered_keys.emplace_back (                 key         );
}

bool
__stdcall
iSK_INI::contains_section (const wchar_t* section)
{
  return sections.count (section) > 0;
}

iSK_INISection&
__stdcall
iSK_INI::get_section (const wchar_t* section)
{
  if (! sections.count (section))
    ordered_sections.emplace_back (section);

  iSK_INISection& ret =
    sections [section];

  ret.name = section;

  return ret;
}

#include <cstdarg>

iSK_INISection&
__stdcall
iSK_INI::get_section_f ( _In_z_ _Printf_format_string_
                         wchar_t const* const    _Format,
                                                 ... )
{
  wchar_t wszFormatted [128] = { };

  int len = 0;

  va_list   _ArgList;
  va_start (_ArgList, _Format);
  {
    // ASSERT: Length <= 127 characters
    len += vswprintf (wszFormatted, _Format, _ArgList);
  }
  va_end   (_ArgList);

  if (! sections.count (wszFormatted))
    ordered_sections.emplace_back (wszFormatted);

  iSK_INISection& ret =
    sections [wszFormatted];

  ret.name = wszFormatted;

  return ret;
}


#include <SpecialK/utility.h>

void
__stdcall
iSK_INI::write (const wchar_t* fname)
{
  SK_CreateDirectories (fname);

  FILE*   fOut = nullptr;
  errno_t ret  = 0;

  switch (encoding_)
  {
    case INI_UTF8:
      TRY_FILE_IO (_wfsopen (fname, L"wtc,ccs=UTF-8",    _SH_DENYNO), fname, fOut);
      break;

    // Cannot preserve this, consider adding a byte-swap on file close
    case INI_UTF16BE:
      TRY_FILE_IO (_wfsopen (fname, L"wtc,ccs=UTF-16LE", _SH_DENYNO), fname, fOut);
      break;

    default:
    case INI_UTF16LE:
      TRY_FILE_IO (_wfsopen (fname, L"wtc,ccs=UTF-16LE", _SH_DENYNO), fname, fOut);
      break;
  }

  if (ret != 0 || fOut == nullptr)
  {
    //SK_MessageBox (L"ERROR: Cannot open INI file for writing. Is it read-only?", fname, MB_OK | MB_ICONSTOP);
    return;
  }



  // Strip Empty Sections
  // --------------------
  //  *** These would cause blank lines to be appended to the end of the INI file
  //        if we did not do something about them here and now. ***
  //
  for ( auto& it : ordered_sections )
  {
    iSK_INISection& section =
      get_section (it.c_str ());

    if (section.ordered_keys.empty ())
    {
      remove_section (section.name.c_str ());
    }
  }


  std::wstring outbuf = L"";


  for ( auto& it : ordered_sections )
  {
    iSK_INISection& section =
      get_section (it.c_str ());

    if (    section.name.length       () &&
         (! section.ordered_keys.empty ()) )
    {
      outbuf += L"[";
      outbuf += section.name + L"]\n";

      for ( auto& key_it : section.ordered_keys )
      {
        const std::wstring& val =
          section.get_value (key_it.c_str ());

        outbuf += key_it + L"=";
        outbuf += val    + L"\n";
      }

      outbuf += L"\n";
    }
  }

  if (outbuf.back () == L'\n')
  {
    // Strip the unnecessary extra newline
    outbuf.resize (outbuf.size () - 1);
  }

  fputws (outbuf.c_str (), fOut);
  fclose (fOut);
}


iSK_INI::_TSectionMap&
__stdcall
iSK_INI::get_sections (void)
{
  return sections;
}


HRESULT
__stdcall
iSK_INI::QueryInterface (THIS_ REFIID riid, void** ppvObj)
{
  if (IsEqualGUID (riid, IID_SK_INI))
  {
    AddRef ();

    *ppvObj = this;

    return S_OK;
  }

  return E_NOTIMPL;
}

ULONG
__stdcall
iSK_INI::AddRef (THIS)
{
  return InterlockedIncrement (&refs);
}

ULONG
__stdcall
iSK_INI::Release (THIS)
{
  return InterlockedDecrement (&refs);
}

bool
__stdcall
iSK_INI::remove_section (const wchar_t* wszSection)
{
  std::wstring section_w (wszSection);

  auto it =
    std::find ( ordered_sections.begin (),
                ordered_sections.end   (),
                  section_w );

  if (it != ordered_sections.end ())
  {
    ordered_sections.erase (it);
    sections.erase         (section_w);

    return true;
  }

  return false;
}

bool
__stdcall
iSK_INISection::remove_key (const wchar_t* wszKey)
{
  std::wstring key_w (wszKey);

  auto it =
    std::find ( ordered_keys.begin (),
                ordered_keys.end   (),
                  key_w );

  if (it != ordered_keys.end ())
  {
    ordered_keys.erase (it);
    keys.erase         (key_w);

    return true;
  }

  return false;
}


HRESULT
__stdcall
iSK_INISection::QueryInterface (THIS_ REFIID riid, void** ppvObj)
{
  if (IsEqualGUID (riid, IID_SK_INISection))
  {
    AddRef ();

    *ppvObj = this;

    return S_OK;
  }

  return E_NOTIMPL;
}

ULONG
__stdcall
iSK_INISection::AddRef (THIS)
{
  return InterlockedIncrement (&refs);
}

ULONG
__stdcall
iSK_INISection::Release (THIS)
{
  ULONG ret = InterlockedDecrement (&refs);

  if (ret == 0)
  {
    // Add to ToFree list in future
    //delete this;
  }

  return ret;
}


const wchar_t*
iSK_INI::get_filename (void) const
{
  return wszName;
}

iSK_INI*
__stdcall
SK_CreateINI (const wchar_t* const wszName)
{
  auto* pINI =
    new iSK_INI (wszName);

  return pINI;
}