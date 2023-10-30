//
// MergeBack routines for the Fast Light Tool Kit (FLTK).
//
// Copyright 1998-2023 by Bill Spitzak and others.
//
// This library is free software. Distribution and use rights are outlined in
// the file "COPYING" which should have been included with this file.  If this
// file is missing or damaged, see the license at:
//
//     https://www.fltk.org/COPYING.php
//
// Please see the following page on how to report bugs and issues:
//
//     https://www.fltk.org/bugs.php
//

#include "mergeback.h"

#include "fluid.h"
#include "code.h"
#include "undo.h"
#include "Fl_Function_Type.h"
#include "Fl_Widget_Type.h"

#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <zlib.h>

extern void propagate_load(Fl_Group*, void*);
extern void load_panel();
extern void redraw_browser();

// TODO: add application user setting to control mergeback
//        [] new projects default to mergeback
//        [] check mergeback when loading project
//        [] check mergeback when app gets focus
//          [] always apply if safe
// TODO: command line option for mergeback
//        -mb or --merge-back
//        -mbs or --merge-back-if-safe
// NOTE: automatic mergeback on timer when file changes if app focus doesn't work
// NOTE: allow the user to edit comment blocks

/**
 Merge external changes in a source code file back into the current project.

 This experimental function reads a source code file line by line. When it
 encounters a special tag in a line, the crc32 stored in the tag is compared
 to the crc32 that was calculated from the code lines since the previous tag.

 If the crc's differ, the user has modified the source file externally, and the
 given block differs from the block as it was generated by FLUID. Depending on
 the block type, the user has modified the widget code (FD_TAG_GENERIC), which
 can not be transferred back into the project.

 Modifications to code blocks and callbacks (CODE, CALLBACK) can be merged back
 into the project. Their corresponding Fl_Type is found using the unique
 node id that is part of the tag. The block is only merged back if the crc's
 from the project and from the edited block differ.

 The caller must make sure that this code file was generated by the currently
 loaded project.

 The user is informed in detailed dialogs what the function discovered and
 offered to merge or cancel if appropriate. Just in case this function is
 destructive, "undo" restores the state before a MergeBack.

 Callers can set different task. FD_MERGEBACK_ANALYSE checks if there are any
 modifications in the code file and returns -1 if there was an error, or a
 bit field where bit 0 is set if internal structures were modified, bit 1 if
 code was changed, and bit 2 if modified blocks were found, but no Type node.
 Bit 3 is set, if code was changed in the code file *and* the project.

 FD_MERGEBACK_INTERACTIVE checks for changes and presents a status dialog box
 to the user if there were conflicting changes or if a mergeback is possible,
 presenting the user the option to merge or cancel. Returns 0 if the project
 remains unchanged, and 1 if the user merged changes back. -1 is returned if an
 invalid tag was found.

 FD_MERGEBACK_APPLY merges all changes back into the project without any
 interaction. Returns 0 if nothing changed, and 1 if it merged any changes back.

 FD_MERGEBACK_APPLY_IF_SAFE merges changes back only if there are no conflicts.
 Returns 0 if nothing changed, and 1 if it merged any changes back, and -1 if
 there were conflicts.

 \note this function is currently part of Fd_Code_Writer to get easy access
 to our crc32 code that also wrote the code file originally.

 \param[in] s path and filename of the source code file
 \param[in] task see above
 \return -1 if an error was found in a tag
 \return -2 if no code file was found
 \return see above
 */
int merge_back(const Fl_String &s, const Fl_String &p, int task) {
  if (g_project.write_mergeback_data) {
    Fd_Mergeback mergeback;
    return mergeback.merge_back(s, p, task);
  } else {
    // nothing to be done if the mergeback option is disabled in the project
    return 0;
  }
}

/** Allocate and initialize MergeBack class. */
Fd_Mergeback::Fd_Mergeback() :
  code(NULL),
  line_no(0),
  tag_error(0),
  num_changed_code(0),
  num_changed_structure(0),
  num_uid_not_found(0),
  num_possible_override(0)
{
}

/** Release allocated resources. */
Fd_Mergeback::~Fd_Mergeback()
{
  if (code) ::fclose(code);
}

/** Remove the first two spaces at every line start.
 \param[inout] s block of C code
 */
void Fd_Mergeback::unindent(char *s) {
  char *d = s;
  bool line_start = true;
  while (*s) {
    if (line_start) {
      if (*s>0 && isspace(*s)) s++;
      if (*s>0 && isspace(*s)) s++;
      line_start = false;
    }
    if (*s=='\r') s++;
    if (*s=='\n') line_start = true;
    *d++ = *s++;
  }
  *d = 0;
}

/**
 Read a block of text from the source file and remove the leading two spaces in every line.
 \param[in] start start of the block within the file
 \param[in] end end of text within the file
 \return a string holding the text that was found in the file
 */
Fl_String Fd_Mergeback::read_and_unindent_block(long start, long end) {
  long bsize = end-start;
  long here = ::ftell(code);
  ::fseek(code, start, SEEK_SET);
  char *block = (char*)::malloc(bsize+1);
  size_t n = ::fread(block, bsize, 1, code);
  if (n!=1)
    block[0] = 0; // read error
  else
    block[bsize] = 0;
  unindent(block);
  Fl_String str = block;
  ::free(block);
  ::fseek(code, here, SEEK_SET);
  return str;
}

/** Tell user the results of our MergeBack analysis and pop up a dialog to give
 the user a choice to merge or cancel.
 \return 1 if the user wants to merge (choice dialog was shown)
 \return 0 if there is nothing to merge (no dialog was shown)
 \return -1 if the user wants to cancel or an error occurred or an issue was presented
        (message or choice dialog was shown)
 */
int Fd_Mergeback::ask_user_to_merge(const Fl_String &code_filename, const Fl_String &proj_filename) {
  if (tag_error) {
    fl_message("Comparing\n  \"%s\"\nto\n  \"%s\"\n\n"
               "MergeBack found an error in line %d while reading tags\n"
               "from the source code. Merging code back is not possible.",
               code_filename.c_str(), proj_filename.c_str(), line_no);
    return -1;
  }
  if (!num_changed_code && !num_changed_structure) {
    return 0;
  }
  if (num_changed_structure && !num_changed_code) {
    fl_message("Comparing\n  \"%1$s\"\nto\n  \"%2$s\"\n\n"
               "MergeBack found %3$d modifications in the project structure\n"
               "of the source code. These kind of changes can no be\n"
               "merged back and will be lost when the source code is\n"
               "generated again from the open project.", 
               code_filename.c_str(), proj_filename.c_str(), num_changed_structure);
    return -1;
  }
  Fl_String msg = "Comparing\n  \"%1$s\"\nto\n  \"%2$s\"\n\n"
                  "MergeBack found %3$d modifications in the source code.";
  if (num_possible_override)
    msg += "\n\nWARNING: %6$d of these modified blocks appear to also have\n"
    "changed in the project. Merging will override changes in\n"
    "the project with changes from the source code file.";
  if (num_uid_not_found)
    msg += "\n\nWARNING: for %4$d of these modifications no Type node\n"
    "can be found and these modification can't be merged back.";
  if (!num_possible_override && !num_uid_not_found)
    msg += "\nMerging these changes back appears to be safe.";

  if (num_changed_structure)
    msg += "\n\nWARNING: %5$d modifications were found in the project\n"
    "structure. These kind of changes can no be merged back\n"
    "and will be lost when the source code is generated again\n"
    "from the open project.";

  if (num_changed_code==num_uid_not_found) {
    fl_message(msg.c_str(),
               code_filename.c_str(), proj_filename.c_str(),
               num_changed_code, num_uid_not_found,
               num_changed_structure, num_possible_override);
    return -1;
  } else {
    msg +=    "\n\nClick Cancel to abort the MergeBack operation.\n"
    "Click Merge to merge all code changes back into\n"
    "the open project.";
    int c = fl_choice(msg.c_str(), "Cancel", "Merge", NULL,
                      code_filename.c_str(), proj_filename.c_str(),
                      num_changed_code, num_uid_not_found,
                      num_changed_structure, num_possible_override);
    if (c==0) return -1;
    return 1;
  }
}

/** Analyse the block and its corresponding widget callback.
 Return findings in num_changed_code, num_changed_code, and num_uid_not_found.
 */
void Fd_Mergeback::analyse_callback(unsigned long code_crc, unsigned long tag_crc, int uid) {
  Fl_Type *tp = Fl_Type::find_by_uid(uid);
  if (tp && tp->is_true_widget()) {
    Fl_String cb = tp->callback(); cb += "\n";
    unsigned long project_crc = Fd_Code_Writer::block_crc(cb.c_str());
    // check if the code and project crc are the same, so this modification was already applied
    if (project_crc!=code_crc) {
      num_changed_code++;
      // check if the block change on the project side as well, so we may override changes
      if (project_crc!=tag_crc) {
        num_possible_override++;
      }
    }
  } else {
    num_uid_not_found++;
    num_changed_code++;
  }
}

/** Analyse the block and its corresponding Code Type.
 Return findings in num_changed_code, num_changed_code, and num_uid_not_found.
 */
void Fd_Mergeback::analyse_code(unsigned long code_crc, unsigned long tag_crc, int uid) {
  Fl_Type *tp = Fl_Type::find_by_uid(uid);
  if (tp && tp->is_a(ID_Code)) {
    Fl_String code = tp->name(); code += "\n";
    unsigned long project_crc = Fd_Code_Writer::block_crc(code.c_str());
    // check if the code and project crc are the same, so this modification was already applied
    if (project_crc!=code_crc) {
      num_changed_code++;
      // check if the block change on the project side as well, so we may override changes
      if (project_crc!=tag_crc) {
        num_possible_override++;
      }
    }
  } else {
    num_changed_code++;
    num_uid_not_found++;
  }
}


/** Analyse the code file and return findings in class member variables.

 The code file must be open for reading already.

 * tag_error is set if a tag was found, but could not be read
 * line_no returns the line where an error occurred
 * num_changed_code is set to the number of changed code blocks in the file.
   Code changes can be merged back to the project.
 * num_changed_structure is set to the number of structural changes.
   Structural changes outside of code blocks can not be read back.
 * num_uid_not_found number of blocks that were modified, but the corresponding
   type or widget can not be found in the project
 * num_possible_override number of blocks that were changed in the code file,
   but also were changed in the project.
 
 \return -1 if reading a tag failed, otherwise 0
 */
int Fd_Mergeback::analyse() {
  // initialize local variables
  unsigned long code_crc = 0;
  bool line_start = true;
  char line[1024];
  // bail if the caller has not opened a file yet
  if (!code) return 0;
  // initialize member variables to return our findings
  line_no = 0;
  tag_error = 0;
  num_changed_code = 0;
  num_changed_structure = 0;
  num_uid_not_found = 0;
  num_possible_override = 0;
  code_crc = 0;
  // loop through all lines in the code file
  ::fseek(code, 0, SEEK_SET);
  for (;;) {
    // get the next line until end of file
    if (fgets(line, 1023, code)==0) break;
    line_no++;
    const char *tag = strstr(line, "//~fl~");
    if (!tag) {
      // if this line has no tag, add the contents to the CRC and continue
      code_crc = Fd_Code_Writer::block_crc(line, -1, code_crc, &line_start);
    } else {
      // if this line has a tag, read all tag data
      int tag_type = -1, uid = 0;
      unsigned long tag_crc = 0;
      int n = sscanf(tag, "//~fl~%d~%04x~%08lx~~", &tag_type, &uid, &tag_crc);
      if (n!=3 || tag_type<0 || tag_type>FD_TAG_LAST ) { tag_error = 1; return -1; }
      if (code_crc != tag_crc) {
        switch (tag_type) {
          case FD_TAG_GENERIC: 
            num_changed_structure++;
            break;
          case FD_TAG_MENU_CALLBACK:
          case FD_TAG_WIDGET_CALLBACK:
            analyse_callback(code_crc, tag_crc, uid);
            break;
          case FD_TAG_CODE:
            analyse_code(code_crc, tag_crc, uid);
            break;
        }
      }
      // reset everything for the next block
      code_crc = 0;
      line_start = true;
    }
  }
  return 0;
}

/** Apply callback mergebacks from the code file to the project.
 \return 1 if the project changed
 */
int Fd_Mergeback::apply_callback(long block_end, long block_start, unsigned long code_crc, int uid) {
  Fl_Type *tp = Fl_Type::find_by_uid(uid);
  if (tp && tp->is_true_widget()) {
    Fl_String cb = tp->callback(); cb += "\n";
    unsigned long project_crc = Fd_Code_Writer::block_crc(cb.c_str());
    if (project_crc!=code_crc) {
      tp->callback(read_and_unindent_block(block_start, block_end).c_str());
      return 1;
    }
  }
  return 0;
}

/** Apply callback mergebacks from the code file to the project.
 \return 1 if the project changed
 */
int Fd_Mergeback::apply_code(long block_end, long block_start, unsigned long code_crc, int uid) {
  Fl_Type *tp = Fl_Type::find_by_uid(uid);
  if (tp && tp->is_a(ID_Code)) {
    Fl_String cb = tp->name(); cb += "\n";
    unsigned long project_crc = Fd_Code_Writer::block_crc(cb.c_str());
    if (project_crc!=code_crc) {
      tp->name(read_and_unindent_block(block_start, block_end).c_str());
      return 1;
    }
  }
  return 0;
}

/** Apply all possible mergebacks from the code file to the project.
 The code file must be open for reading already.
 \return -1 if reading a tag failed, 0 if nothing changed, 1 if the project changed
 */
int Fd_Mergeback::apply() {
  // initialize local variables
  unsigned long code_crc = 0;
  bool line_start = true;
  char line[1024];
  int changed = 0;
  long block_start = 0;
  long block_end = 0;
  // bail if the caller has not opened a file yet
  if (!code) return 0;
  // initialize member variables to return our findings
  line_no = 0;
  tag_error = 0;
  code_crc = 0;
  // loop through all lines in the code file
  ::fseek(code, 0, SEEK_SET);
  for (;;) {
    // get the next line until end of file
    if (fgets(line, 1023, code)==0) break;
    line_no++;
    const char *tag = strstr(line, "//~fl~");
    if (!tag) {
      // if this line has no tag, add the contents to the CRC and continue
      code_crc = Fd_Code_Writer::block_crc(line, -1, code_crc, &line_start);
      block_end = ::ftell(code);
    } else {
      // if this line has a tag, read all tag data
      int tag_type = -1, uid = 0;
      unsigned long tag_crc = 0;
      int n = sscanf(tag, "//~fl~%d~%04x~%08lx~~", &tag_type, &uid, &tag_crc);
      if (n!=3 || tag_type<0 || tag_type>FD_TAG_LAST ) { tag_error = 1; return -1; }
      if (code_crc != tag_crc) {
        if (tag_type==FD_TAG_MENU_CALLBACK || tag_type==FD_TAG_WIDGET_CALLBACK) {
          changed |= apply_callback(block_end, block_start, code_crc, uid);
        } else if (tag_type==FD_TAG_CODE) {
          changed |= apply_code(block_end, block_start, code_crc, uid);
        }
      }
      // reset everything for the next block
      code_crc = 0;
      line_start = true;
      block_start = ::ftell(code);
    }
  }
  return changed;
}

/** Dispatch the MergeBack into analysis, interactive, or apply directly.
 \param[in] s source code filename and path
 \param[in] task one of FD_MERGEBACK_ANALYSE, FD_MERGEBACK_INTERACTIVE,
            FD_MERGEBACK_APPLY_IF_SAFE, or FD_MERGEBACK_APPLY
 \return -1 if an error was found in a tag
 \return -2 if no code file was found
 \return See more at ::merge_back(const Fl_String &s, int task).
 */
int Fd_Mergeback::merge_back(const Fl_String &s, const Fl_String &p, int task) {
  int ret = 0;
  code = fl_fopen(s.c_str(), "r");
  if (!code) return -2;
  do { // no actual loop, just make sure we close the code file
    if (task == FD_MERGEBACK_ANALYSE) {
      analyse();
      if (tag_error) {ret = -1; break; }
      if (num_changed_structure) ret |= 1;
      if (num_changed_code) ret |= 2;
      if (num_uid_not_found) ret |= 4;
      if (num_possible_override) ret |= 8;
      break;
    }
    if (task == FD_MERGEBACK_INTERACTIVE) {
      analyse();
      ret = ask_user_to_merge(s, p);
      if (ret != 1)
        return ret;
      task = FD_MERGEBACK_APPLY; // fall through
    }
    if (task == FD_MERGEBACK_APPLY_IF_SAFE) {
      analyse();
      if (tag_error || num_changed_structure || num_possible_override) {
        ret = -1;
        break;
      }
      if (num_changed_code==0) {
        ret = 0;
        break;
      }
      task = FD_MERGEBACK_APPLY; // fall through
    }
    if (task == FD_MERGEBACK_APPLY) {
      ret = apply();
      if (ret == 1) {
        set_modflag(1);
        redraw_browser();
        load_panel();
      }
      ret = 1; // avoid message box in caller
    }
  } while (0);
  fclose(code);
  code = NULL;
  return ret;
}