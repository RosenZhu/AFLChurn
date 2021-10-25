
/* get distribution of programs: age and churn */

// #define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

#include <string.h>
#include <set>
#include <map>
#include <cmath>


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <iostream>
#include <fstream>
// #include <string>
#include <sstream>
#include <list>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#include <dirent.h>


using namespace std;

#define DISTRIBUTION_CHANGE 1000
#define DISTRIBUTION_AGE    8000
#define DISTRIBUTION_RANK   (1<<16)


/* Distribution: 
    The number of lines whose #changes are the index.
    e.g., "distNumChange[5] = 100" means that 
      there are 100 lines whose #changes are 5*/
static unsigned int distNumChange[DISTRIBUTION_CHANGE];
/* For age. */
static unsigned int distNumAge[DISTRIBUTION_AGE];
/* For rank */
static unsigned int distNumRank[DISTRIBUTION_RANK];

static unsigned int total_lines_age = 0, total_lines_changes = 0;

bool startsWith(std::string big_str, std::string small_str){
  if (big_str.compare(0, small_str.length(), small_str) == 0) return true;
  else return false;
}

// double inst_norm_age(int max_days, int days_since_last_change){
//   double norm_days;
//   // if (days_since_last_change < 0) norm_days = 1;
//   // else norm_days = 
//   //     1 / (log2(days_since_last_change + 2) * log2(days_since_last_change + 2));

//   /* Normalize 1/days */
//   if (days_since_last_change <= 0 || max_days <= 1) {
//     norm_days = 1;
//     WARNF("Current days are less than 0 or maximum days are less than 1.");
//   }
//   else{
//     norm_days = (double)(max_days - days_since_last_change) / 
//                             (days_since_last_change * (max_days - 1));
//   }

//   return norm_days;

// }

// double inst_norm_rank(int max_rank, int line_rank){
//   double norm_ranks;
//   // rlogrank
//   // if (line_rank < 0) norm_ranks = 1;
//   // else norm_ranks = 1 / log2(line_rank + 2);

//   // log2rank
//   // if (max_rank >= 1){
//   //   if (line_rank < 0) norm_ranks = 1;
//   //   else norm_ranks = (log2(max_rank + 1) - log2(line_rank + 1)) / log2(max_rank + 1);
//   // }

//   /* rrank */
//   if (line_rank <= 0) {
//     norm_ranks = 1;
//     WARNF("Rank of lines is less than 0.");
//   }
//   else norm_ranks = 1 / (double)line_rank;

//   return norm_ranks;

// }

// double inst_norm_change(unsigned int num_changes, unsigned short change_select){
//   double norm_chg = 0;

//   switch(change_select){
//     case CHURN_LOG_CHANGE:
//       // logchanges
//       if (num_changes < 0) norm_chg = 0;
//       else norm_chg = log2(num_changes + 1);
//       break;

//     case CHURN_CHANGE:
//       norm_chg = num_changes;
//       break;

//     case CHURN_CHANGE2:
//       // change^2
//       norm_chg = (double)num_changes * num_changes;
//       break;
//     default:
//       FATAL("Wrong CHURN_CHANGE type!");
//   }
//   // // logchanges
//   // if (num_changes < 0) norm_chg = 0;
//   // else norm_chg = log2(num_changes + 1);

//   // // change^2
//   // norm_chg = num_changes * num_changes;
    
//   // // xlogchange
//   //   if (num_changes < 0) norm_chg = 0;
//   //   else norm_chg = (num_changes + 1) * log2(num_changes + 1);

//   return norm_chg;

// }


/* use popen() to execute git command */
std::string execute_git_cmd (std::string directory, std::string str_cmd){
  FILE *fp;
  int rc=0;
  std::string str_res = "";
  char ch_git_res[2048];
  std::ostringstream git_cmd;
  git_cmd << "cd " 
          << directory
          << " && "
          << str_cmd;
  fp = popen(git_cmd.str().c_str(), "r");
	if(NULL == fp) return str_res;
	// when cmd fail, output "fatal: ...";
  // when succeed, output result
  if (fscanf(fp, "%s", ch_git_res) == 1) {
    str_res.assign(ch_git_res);  //, strlen(ch_git_res)
  }

  if (startsWith(str_res, "fatal")){
    str_res = "";
  }

  rc = pclose(fp);
  if(-1 == rc){
    printf("git command pclose() fails\n");
  }

  return str_res;

}

// /* Get the number of changes that is used to choose between "always instrument"
//         or "insturment randomly".
// Note: #changes of a file is always larger than #changes of a line in the file,
//     so THRESHOLD_PERCENT_CHANGES is set in a low value. */
// int get_threshold_changes(std::string directory){

//   std::ostringstream changecmd;
//   unsigned int largest_changes = 0;
//   int change_threshold = 0;
//   FILE *dfp;
//   // The maximum number of changes to any file.
//   changecmd << "cd " << directory
//           << " && git log --name-only --pretty=\"format:\""
//           << " | sed '/^\\s*$/d' | sort | uniq -c | sort -n"
//           << " | tr -s ' ' | sed \"s/^ //g\" | cut -d\" \" -f1 | tail -n1";
//   dfp = popen(changecmd.str().c_str(), "r");
//   if(NULL == dfp) return WRONG_VALUE;

//   if (fscanf(dfp, "%u", &largest_changes) != 1) return WRONG_VALUE;

//   change_threshold = (THRESHOLD_PERCENT_CHANGES * largest_changes) / 100;

//   pclose(dfp);

//   return change_threshold;
// }

/* if return value is WRONG_VALUE, something wrong happens */
int get_commit_time_days(std::string directory, std::string git_cmd){
  unsigned long unix_time = 0;
  FILE *dfp;
  std::ostringstream datecmd;

  datecmd << "cd " << directory
          << " && "
          << git_cmd;
  dfp = popen(datecmd.str().c_str(), "r");

  if (NULL == dfp) return WRONG_VALUE;
  if (fscanf(dfp, "%lu", &unix_time) != 1){
    pclose(dfp);
    return WRONG_VALUE;
  } 

  pclose(dfp);

  return unix_time / 86400;

}

/* Get the number of commits before HEAD;
  if return value is WRONG_VALUE, something wrong happens */
int get_max_ranks(std::string git_directory){

  FILE *dfp;
  unsigned int head_num_parents;
  std::ostringstream headcmd;
  headcmd << "cd " << git_directory
          << " && git rev-list --count HEAD";
  dfp = popen(headcmd.str().c_str(), "r");
  if (NULL == dfp) return WRONG_VALUE;
  if (fscanf(dfp, "%u", &head_num_parents) != 1){
    pclose(dfp);
    return WRONG_VALUE;
  } 

  pclose(dfp);

  return head_num_parents;

}

/* 
 git diff current_commit HEAD -- file_path. 
 Help get the related lines in HEAD commits, which are related to the lines from git show.
 */
void git_diff_current_head(std::string cur_commit_sha, std::string git_directory, 
            std::string relative_file_path, std::set<unsigned int> &changed_lines_from_show,
                std::map <unsigned int, unsigned int> &lines2changes){

    std::ostringstream cmd;
    char array_head_changes[32], array_current_changes[32], fatat[12], tatat[12];
    std::string current_line_range, head_line_result;
    size_t cur_comma_pos, head_comma_pos;
    int rc = 0;
    FILE *fp;
    int cur_line_num, cur_num_start, cur_num_count, head_line_num, head_num_start, head_num_count;
    std::set<unsigned int> cur_changed_lines, head_changed_lines;
    bool is_head_changed = false, cur_head_has_diff = false;

    /* git diff -U0 cur_commit HEAD -- filename | grep ...
      get the changed line range between current commit and HEAD commit;
      help get the changed lines in HEAD commits;
      result: "@@ -8,0 +9,2 @@"
            (-): current commit; (+): HEAD commit
    */
    cmd << "cd " << git_directory << " && git diff -U0 " << cur_commit_sha << " HEAD -- " << relative_file_path
        << " | grep -o -P \"^@@ -[0-9]+(,[0-9])? \\+[0-9]+(,[0-9])? @@\"";

    fp = popen(cmd.str().c_str(), "r");
    if(NULL == fp) return;
    /* -: current_commit;
       +: HEAD */
    // result: "@@ -8,0 +9,2 @@" or "@@ -10 +11,0 @@" or "@@ -466,8 +475 @@" or "@@ -8 +9 @@"
    while(fscanf(fp, "%s %s %s %s", fatat, array_current_changes, array_head_changes, tatat) == 4){

        cur_head_has_diff = true;
        
        current_line_range.clear(); /* The current commit side, (-) */
        current_line_range.assign(array_current_changes); // "-"
        current_line_range.erase(0,1); //remove "-"
        cur_comma_pos = current_line_range.find(",");
        /* If the changed lines in current commit can be found in changed_lines_from_show, 
            the related lines in HEAD commit should count for changes. */
        if (cur_comma_pos == std::string::npos){
            cur_line_num = std::stoi(current_line_range);
            if (changed_lines_from_show.count(cur_line_num)) is_head_changed = true;
        }else{
            cur_num_start = std::stoi(current_line_range.substr(0, cur_comma_pos));
            cur_num_count = std::stoi(current_line_range.substr(cur_comma_pos + 1, 
                                                                current_line_range.length() - cur_comma_pos - 1));
            for(int i=0; i< cur_num_count; i++){
                if (changed_lines_from_show.count(cur_num_start + i)){
                    is_head_changed = true;
                    break;
                }
            }
        }

        /* Trace changes for head commit, increment lines2changes.
          Some lines are changed in current commit, so trace these lines back to HEAD commit,
          and increment the count of these lines in HEAD commit. 
          */
        if (is_head_changed){
            head_line_result.clear(); /* The head commit side, (+) */
            head_line_result.assign(array_head_changes); // "+"
            head_line_result.erase(0,1); //remove "+"
            head_comma_pos = head_line_result.find(",");

            if (head_comma_pos == std::string::npos){
                head_line_num = std::stoi(head_line_result);
                if (lines2changes.count(head_line_num)) lines2changes[head_line_num]++;
                else lines2changes[head_line_num] = 1;
            }else{
                head_num_start = std::stoi(head_line_result.substr(0, head_comma_pos));
                head_num_count = std::stoi(head_line_result.substr(head_comma_pos + 1, 
                                                                  head_line_result.length() - head_comma_pos - 1));
                for(int i=0; i< head_num_count; i++){ 
                    if (lines2changes.count(head_num_start + i)) lines2changes[head_num_start + i]++;
                    else lines2changes[head_num_start + i] = 1; 
                }
            }

        }

        memset(array_current_changes, 0, sizeof(array_current_changes));
        memset(array_head_changes, 0, sizeof(array_head_changes));
    }

    /* if there's no diff in current commit and HEAD commit;
     there's no change of the file between two commits;
     so any change in current commit (compared to its parents) counts for the HEAD commit.
     CHECK: If an empty line is added, there's no diff result. 
     But the line number in current commit and HEAD commit are different.*/
    /*
    if (!cur_head_has_diff){
        for (auto mit = changed_lines_from_show.begin(); mit != changed_lines_from_show.end(); ++mit){
            if (lines2changes.count(*mit)) lines2changes[*mit]++;
            else lines2changes[*mit] = 1;
        }
    }*/

    rc = pclose(fp);
    if(-1 == rc){
        printf("git diff pclose() fails\n");
    }
}

/* git show, get changed lines in current commit.
    It'll show you the log message for the commit, and the diff of that particular commit.
    Find the changed line numbers in file relative_file_path as it was changed in commit cur_commit_sha, 
    and add them to the list changed_lines_cur_commit     
 */
void git_show_current_changes(std::string cur_commit_sha, std::string git_directory, 
            std::string relative_file_path, std::set<unsigned int> &changed_lines_cur_commit){

    std::ostringstream cmd;
    
    char array_parent_changes[32], array_current_changes[32], fatat[12], tatat[12];
    std::string current_line_range;
    size_t comma_pos;
    int rc = 0;
    FILE *fp;
    int line_num, num_start, num_count; 

    // git show: parent_commit(-) current_commit(+)
    // result: "@@ -8,0 +9,2 @@" or "@@ -10 +11,0 @@" or "@@ -466,8 +475 @@" or "@@ -8 +9 @@"
    cmd << "cd " << git_directory << " && git show --oneline -U0 " << cur_commit_sha << " -- " << relative_file_path
          << " | grep -o -P \"^@@ -[0-9]+(,[0-9])? \\+[0-9]+(,[0-9])? @@\"";

    fp = popen(cmd.str().c_str(), "r");
    if(NULL == fp) return;
    // get numbers in (+): current commit
    
    while(fscanf(fp, "%s %s %s %s", fatat, array_parent_changes, array_current_changes, tatat) == 4){

      current_line_range.clear(); /* The current commit side, (+) */
      current_line_range.assign(array_current_changes); // "+"
      current_line_range.erase(0,1); //remove "+"
      comma_pos = current_line_range.find(",");

      if (comma_pos == std::string::npos){
          line_num = std::stoi(current_line_range);
          if (line_num >= 0) changed_lines_cur_commit.insert(line_num);
      }else{
          num_start = std::stoi(current_line_range.substr(0, comma_pos));
          num_count = std::stoi(current_line_range.substr(comma_pos+1, 
                                                          current_line_range.length() - comma_pos - 1));
          for(int i = 0; i< num_count; i++){
              if (num_start >= 0) changed_lines_cur_commit.insert(num_start + i);
          }
      }
      memset(array_current_changes, 0, sizeof(array_current_changes));
    }

    rc = pclose(fp);
    if(-1 == rc){
        printf("git show pclose() fails\n");
    }
}

// /* use git command to get line changes */
// void calculate_line_change_git_cmd(std::string relative_file_path, std::string git_directory,
//                     std::map<std::string, std::map<unsigned int, double>> &file2line2change_map,
//                     unsigned short change_sig){
    
//   std::ostringstream cmd;
//   std::string str_cur_commit_sha;
//   char ch_cur_commit_sha[128];
//   int rc = 0;
//   FILE *fp;
//   std::set<unsigned int> changed_lines_cur_commit;
//   std::map <unsigned int, unsigned int> lines2changes;
//   std::map <unsigned int, double> tmp_line2changes;
  
//   // get the commits that change the file of relative_file_path
//   // result: commit short SHAs
//   // TODO: If the file name changed, it cannot get the changed lines.
//   //  --since=10.years 
//   char* ch_month = getenv("AFLCHURN_SINCE_MONTHS");
//   if (ch_month){
//     std::string since_month(ch_month);
//     if (since_month.find_first_not_of("0123456789") == std::string::npos){ // all digits
      
//       cmd << "cd " << git_directory 
//           << " && git log --since=" << ch_month << ".months"
//           << " --follow --oneline --format=\"%h\" -- " 
//           << relative_file_path << " | grep -Po \"^[0-9a-f]*$\"";
          
//     } else {
//       ch_month = NULL; // if env variable is not digits, use all commits
//     }
//   }
  
//   if (!ch_month){

//     cmd << "cd " << git_directory 
//         << " && git log" 
//         <<" --follow --oneline --format=\"%h\" -- " 
//         << relative_file_path
//         << " | grep -Po \"^[0-9a-f]*$\""; 
//   }
  
//   fp = popen(cmd.str().c_str(), "r");
//   if(NULL == fp) return;
//   /* get lines2changes: git log -> git show -> git diff
//     "git log -- filename": get commits SHAs changing the file
//     "git show $commit_sha -- filename": get changed lines in current commit
//     "git diff $commit_sha HEAD -- filename": get the related lines in HEAD commit
//     */
//   while(fscanf(fp, "%s", ch_cur_commit_sha) == 1){
//       str_cur_commit_sha.clear();
//       str_cur_commit_sha.assign(ch_cur_commit_sha);
//       // get changed_lines_cur_commit: the change lines in current commit
//       changed_lines_cur_commit.clear();
//       git_show_current_changes(str_cur_commit_sha, git_directory, 
//                                   relative_file_path, changed_lines_cur_commit);
//       // get lines2changes: related change lines in HEAD commit
//       git_diff_current_head(str_cur_commit_sha, git_directory, relative_file_path, 
//                               changed_lines_cur_commit, lines2changes);
      
//   }

//   /* Get changes */
//   if (!lines2changes.empty()){
//     // logchanges
//     for (auto l2c : lines2changes){
//       tmp_line2changes[l2c.first] = inst_norm_change(l2c.second, change_sig);
//     }

//     file2line2change_map[relative_file_path] = tmp_line2changes;
    
//   }
  
//   rc = pclose(fp);
//   if(-1 == rc){
//       printf("git log pclose() fails\n");
//   }

// }


// /* get age of lines using git command line. 
//   git_directory: /home/usrname/repo/
//   head_commit_days: unix time of head commit, in days;
//   init_commit_days: unix time of initial 
// */
// bool calculate_line_age_git_cmd(std::string relative_file_path, std::string git_directory,
//                     std::map<std::string, std::map<unsigned int, double>> &file2line2age_map,
//                     int head_commit_days, int init_commit_days){

//   std::map<unsigned int, double> line_age_days;

//   /*
//   getting pairs [unix_time line_number]
//   // cd ${git_directory} &&
//   // git blame --date=unix ${relative_file_path} \
//   // | grep -o -P "[0-9]{9}[0-9]? [0-9]+"
//   */

//   std::ostringstream cmd;
//   int rc = 0;
//   FILE *fp;
//   unsigned long unix_time;
//   unsigned int line;
//   int days_since_last_change;

//   if (head_commit_days==WRONG_VALUE || init_commit_days==WRONG_VALUE) return false;

//   int max_days = head_commit_days - init_commit_days;

//   cmd << "cd " << git_directory << " && git blame --date=unix " << relative_file_path
//         << " | grep -o -P \"[0-9]{9}[0-9]? +[0-9]+\"";

//   fp = popen(cmd.str().c_str(), "r");
//   if(NULL == fp) return false;
//   // get line by line
//   while(fscanf(fp, "%lu %u", &unix_time, &line) == 2){
//     days_since_last_change = head_commit_days - unix_time / 86400; //days

//     line_age_days[line] = inst_norm_age(max_days, days_since_last_change);
    
//   }

//   if (!line_age_days.empty())
//       file2line2age_map[relative_file_path] = line_age_days;

//   rc = pclose(fp);
//   if(-1 == rc){
//     printf("git blame pclose() fails\n");
//   }

//   return true;

// }

// /* get rank of line ages.
//   rank = (the number of commits until HEAD) - (the number of commits until commit A);
//  */
// bool cal_line_age_rank(std::string relative_file_path, std::string git_directory,
//                 std::map<std::string, std::map<unsigned int, double>> &file2line2rank_map,
//                 std::map<std::string, double> &commit2rank,
//                 int head_num_parents){

//   char line_commit_sha[256];
//   FILE *dfp, *curdfp;
//   unsigned int nothing_line, line_num;
//   std::map<unsigned int, double> line_rank;
//   unsigned int cur_num_parents;
//   int rank4line;

//   if (head_num_parents == WRONG_VALUE) return false;

//   /* output: commit_hash old_line_num current_line_num
//         e.g., 9f1a353f68d6586b898c47c71a7631cdc816215f 167 346
//    */
//   std::ostringstream blamecmd;
//   blamecmd << "cd " << git_directory
//         << " && git blame -p -- " << relative_file_path
//         << " | grep -o \"^[0-9a-f]* [0-9]* [0-9]*\"";

//   dfp = popen(blamecmd.str().c_str(), "r");
//   if(NULL == dfp) return false;

//   std::ostringstream rankcmd;
//   while (fscanf (dfp, "%s %u %u", line_commit_sha, &nothing_line, &line_num) == 3){
//     std::string str_cmt(line_commit_sha);
//     if (commit2rank.count(str_cmt)){
//       line_rank[line_num] = commit2rank[str_cmt];
//     } else {
//       rankcmd.str("");
//       rankcmd.clear();
//       rankcmd << "cd " << git_directory
//               << " && git rev-list --count "
//               << line_commit_sha;
//       curdfp = popen(rankcmd.str().c_str(), "r");
//       if(NULL == curdfp) continue;
//       if (fscanf (curdfp, "%u", &cur_num_parents) == 1){
//         rank4line = head_num_parents - cur_num_parents;
//         commit2rank[str_cmt] = line_rank[line_num] 
//                              = inst_norm_rank(head_num_parents, rank4line);
//       }
//       pclose(curdfp);
//     }
    
//   }
//   pclose(dfp);

//   if (!line_rank.empty()) file2line2rank_map[relative_file_path] = line_rank;
//   return true;
  
// }



/* Check if file exists in HEAD using command mode.
return:
    exist: 1; not exist: 0 */
bool is_file_exist(std::string relative_file_path, std::string git_directory){

  //string cmd("cd /home/usrname/repo && git cat-file -e HEAD:util/read.h 2>&1");
  std::ostringstream cmd;

  char result_buf[1024];
  int rc = 0;
  bool isSuccess = false;
  FILE *fp;

  if(access(git_directory.c_str(), F_OK) == -1) return false;
  
  cmd << "cd " << git_directory << " && git cat-file -e HEAD:" 
      << relative_file_path << " 2>&1";

	fp = popen(cmd.str().c_str(), "r");
	if(NULL == fp) return false;
	// when cmd fail, output "fatal: Path 'tdio.h' does not exist in 'HEAD'";
  // when succeed, output nothing
  if (fgets(result_buf, sizeof(result_buf), fp) != NULL) isSuccess = false;
  else isSuccess = true;
	
  rc = pclose(fp);
  if(-1 == rc){
    printf("git cat-file pclose() fails\n");
  }
  
  return isSuccess;

}


// /* Change the filename to relative path (relative to souce dir) without "../" or "./" in the path.
// Input:
//   relative_file_path: relative path of source files, relative to base_directory
//   base_directory: absolute path of directories in building directory
//   git_directory: absolute path of git repo directory (root of source code)
// Output:
//   clean relative path of a file
//  */
// std::string get_file_path_relative_to_git_dir(std::string relative_file_path, 
//                     std::string base_directory, std::string git_directory){

//     std::string clean_relative_path;

  
//   if (startsWith(relative_file_path, "/")){
//     // "/path/to/configure": relative_file_path = /path/to/file.c
//     // remove substring, which is the same as git_directory, from relative_file_path
//     relative_file_path.erase(0, git_directory.length());  // relative path
//     clean_relative_path = relative_file_path;
//   } else{
//     // "../configure" or "./configure"
//     // relative_file_path could be src/file.c, build/../src/file.c, or src/./file.c
//     // relative_file_path is relative to base_directory here
//     base_directory.append("/");
//     base_directory.append(relative_file_path);
//     // remove "../" or "./"
//     char* resolved_path = realpath(base_directory.c_str(), NULL);
//     //TODO: why is it NULL?
//     if (resolved_path == NULL) clean_relative_path = "";
//     else{
//       clean_relative_path.append(resolved_path);

//       free(resolved_path);

//       clean_relative_path.erase(0, git_directory.length());  // relative path
//     }  
//   }

//   return clean_relative_path;

// }

/* Distribution: get rank of line ages.
  rank = (the number of commits until HEAD) - (the number of commits until commit A);
 */
void get_line_rank_dist(std::string relative_file_path, std::string git_directory,
                int head_num_parents){

  char line_commit_sha[256];
  FILE *dfp, *curdfp;
  unsigned int nothing_line, line_num;
  // std::map<unsigned int, double> line_rank;
  std::map<std::string, int> commit2rank;
  unsigned int cur_num_parents;
  int rank4line;

  if (head_num_parents == WRONG_VALUE) return;

  /* output: commit_hash old_line_num current_line_num
        e.g., 9f1a353f68d6586b898c47c71a7631cdc816215f 167 346
   */
  std::ostringstream blamecmd;
  blamecmd << "cd " << git_directory
        << " && git blame -p -- " << relative_file_path
        << " | grep -o \"^[0-9a-f]* [0-9]* [0-9]*\"";

  dfp = popen(blamecmd.str().c_str(), "r");
  if(NULL == dfp) return;

  std::ostringstream rankcmd;
  while (fscanf (dfp, "%s %u %u", line_commit_sha, &nothing_line, &line_num) == 3){
    std::string str_cmt(line_commit_sha);
    if (commit2rank.count(str_cmt)){
      distNumRank[commit2rank[str_cmt]]++; //distNumRank[DISTRIBUTION_RANK]
    } else {
      rankcmd.str("");
      rankcmd.clear();
      rankcmd << "cd " << git_directory
              << " && git rev-list --count "
              << line_commit_sha;
      curdfp = popen(rankcmd.str().c_str(), "r");
      if(NULL == curdfp) continue;
      if (fscanf (curdfp, "%u", &cur_num_parents) == 1){
        rank4line = head_num_parents - cur_num_parents;
        if (rank4line <= 0) {
          distNumRank[0]++;
          commit2rank[str_cmt] = 0;
        } else if (rank4line < DISTRIBUTION_RANK){
          distNumRank[rank4line]++;
          commit2rank[str_cmt] = rank4line;
        } else{
          distNumRank[DISTRIBUTION_RANK - 1]++;
          commit2rank[str_cmt] = DISTRIBUTION_RANK - 1;
        }

      }
      pclose(curdfp);
    }
    
  }
  pclose(dfp);
  
}



// std::map<std::string, std::map<unsigned int, double>> &file2line2age_map,
/* For distribution of age:
  get age of lines using git command line. 
  git_directory: /home/usrname/repo/
  head_commit_days: unix time of head commit, in days;
  init_commit_days: unix time of initial 
*/
void get_line_age_dist(std::string relative_file_path, std::string git_directory,
                    int head_commit_days){

  std::map<unsigned int, double> line_age_days;

  /*
  getting pairs [unix_time line_number]
  // cd ${git_directory} &&
  // git blame --date=unix ${relative_file_path} \
  // | grep -o -P "[0-9]{9}[0-9]? [0-9]+"
  */

  std::ostringstream cmd;
  int rc = 0;
  FILE *fp;
  unsigned long unix_time;
  unsigned int line;
  int days_since_last_change;

  if (head_commit_days==WRONG_VALUE) return;

  // int max_days = head_commit_days - init_commit_days;

  cmd << "cd " << git_directory << " && git blame --date=unix " << relative_file_path
        << " | grep -o -P \"[0-9]{9}[0-9]? +[0-9]+\"";

  fp = popen(cmd.str().c_str(), "r");
  if(NULL == fp) return;
  // get line by line
  while(fscanf(fp, "%lu %u", &unix_time, &line) == 2){
    days_since_last_change = head_commit_days - unix_time / 86400; //days

    if (days_since_last_change <=0) distNumAge[0]++;
    else if (days_since_last_change < DISTRIBUTION_AGE) distNumAge[days_since_last_change]++;
    else distNumAge[DISTRIBUTION_AGE - 1]++;

    total_lines_age++;
    
  }

  rc = pclose(fp);
  if(-1 == rc){
    printf("git blame pclose() fails\n");
  }

}


//, std::map<std::string, std::map<unsigned int, double>> &file2line2change_map
/* For distribution of #changes.
 use git command to count line's #changes */
void get_line_change_dist(std::string relative_file_path, std::string git_directory){
    
  std::ostringstream cmd;
  std::string str_cur_commit_sha;
  char ch_cur_commit_sha[128];
  int rc = 0;
  FILE *fp;
  std::set<unsigned int> changed_lines_cur_commit;
  std::map <unsigned int, unsigned int> lines2changes; // first = line, second = #changes
  // std::map <unsigned int, double> tmp_line2changes;
  
  // get the commits that change the file of relative_file_path
  // result: commit short SHAs
  // TODO: If the file name changed, it cannot get the changed lines.

  cmd << "cd " << git_directory 
        << " && git log" 
        <<" --follow --oneline --format=\"%h\" -- " 
        << relative_file_path
        << " | grep -Po \"^[0-9a-f]*$\""; 
  
  fp = popen(cmd.str().c_str(), "r");
  if(NULL == fp) return;
  /* get lines2changes: git log -> git show -> git diff
    "git log -- filename": get commits SHAs changing the file
    "git show $commit_sha -- filename": get changed lines in current commit
    "git diff $commit_sha HEAD -- filename": get the related lines in HEAD commit
    */
  while(fscanf(fp, "%s", ch_cur_commit_sha) == 1){
      str_cur_commit_sha.clear();
      str_cur_commit_sha.assign(ch_cur_commit_sha);
      // get changed_lines_cur_commit: the change lines in current commit
      changed_lines_cur_commit.clear();
      git_show_current_changes(str_cur_commit_sha, git_directory, 
                                  relative_file_path, changed_lines_cur_commit);
      // get lines2changes: related change lines in HEAD commit
      git_diff_current_head(str_cur_commit_sha, git_directory, relative_file_path, 
                              changed_lines_cur_commit, lines2changes);
      
  }

  /* Get changes before HEAD */
  if (!lines2changes.empty()){
    for (auto l2c : lines2changes){
      if (l2c.second >= 1 && l2c.second < DISTRIBUTION_CHANGE){
        distNumChange[l2c.second]++;
        total_lines_changes++;
      } else{
        distNumChange[DISTRIBUTION_CHANGE - 1]++;
        total_lines_changes++;
      }

    } 
  }

  /* Get changes in HEAD commit: git show HEAD */
  changed_lines_cur_commit.clear();
  git_show_current_changes("HEAD", git_directory, 
                                  relative_file_path, changed_lines_cur_commit);
  if (!changed_lines_cur_commit.empty()){
    for (auto lineh : changed_lines_cur_commit){
      if (lineh >=1 && lineh < DISTRIBUTION_CHANGE){
        distNumChange[lineh]++;
        total_lines_changes++;
      }else {
        distNumChange[DISTRIBUTION_CHANGE - 1]++;
        total_lines_changes++;
      }
    }
  }
  
  rc = pclose(fp);
  if(-1 == rc){
      printf("git log pclose() fails\n");
  }

}


void listFilesGetDistribution(std::string git_dir, std::string relative_dir,
                              int head_commit_days, int max_num_ranks){
  std::string cur_rela_path;
  struct dirent *dp = NULL;
  DIR *dir = NULL;
  std::string abs_dir, abs_file_path;
  struct stat filestat;
  
  if (relative_dir.empty()){
    abs_dir = git_dir;
  } else {
    abs_dir = git_dir + "/" + relative_dir;
  }
  
  dir = opendir(abs_dir.c_str());

  // fail to open directory
  if (!dir) return;

  while ((dp = readdir(dir)) != NULL){
    std::string fname = dp->d_name;
    
    if (!startsWith(fname, ".")){

      if (relative_dir.empty()){
        cur_rela_path = fname;
      } else {
        cur_rela_path = relative_dir + "/" + fname;
      }
      // cout << "current relative path: " << cur_rela_path << endl;

      abs_file_path = abs_dir + "/" + fname;
      stat(abs_file_path.c_str(), &filestat);
      if (S_ISDIR(filestat.st_mode)){ // directory
        listFilesGetDistribution(git_dir, cur_rela_path, head_commit_days, max_num_ranks);

      } else {// file
        // cout << "it's a file: " << abs_file_path << endl;

        // file not in HEAD commit
        if (!is_file_exist(cur_rela_path, git_dir)) continue;

        /* Get distribution of #changes */
        get_line_change_dist(cur_rela_path, git_dir);

        /* Get distribution of age */
        
        get_line_age_dist(cur_rela_path, git_dir, head_commit_days);

        /* Get distribution of #ranks */
        get_line_rank_dist(cur_rela_path, git_dir, max_num_ranks);

        // break;

      }

      
    }
  }

  closedir(dir);
}


int main(int argc, char *argv[]){


  std::string str_cur_workp;
  struct dirent *dp;
  int head_commit_days, // head commit in unix time, days
      max_num_ranks;   // #commits before HEAD
  int num_keep_lines_age, num_keep_ranks, num_keep_lines_change;
  int tmp_ages = 0, tmp_changes = 0, tmp_ranks = 0;
  int thd_age = 0, thd_change = 0, thd_rank = 0; // threasholds

  char *cur_workp = getcwd(NULL, 0);

  if (cur_workp){
    str_cur_workp.assign(cur_workp);
    
    cout << "Current working directory: " << str_cur_workp << endl;
    std::string cmd_repo ("git rev-parse --show-toplevel 2>&1");
    std::string git_path = execute_git_cmd(str_cur_workp, cmd_repo);
    std::cout << "git path: "<< git_path << std::endl;

    free(cur_workp);

    // get distribution for age and churn
    std::string head_cmd("git show -s --format=%ct HEAD");
    head_commit_days = get_commit_time_days(git_path, head_cmd);
    max_num_ranks = get_max_ranks(git_path);

    listFilesGetDistribution(git_path, "", head_commit_days, max_num_ranks);

    num_keep_lines_change = total_lines_changes * THRESHOLD_PERCENT_CHANGES / 100;
    for (int i = DISTRIBUTION_CHANGE - 1; i > 0 ; i--){
      if (distNumChange[i] != 0) {
        cout << i << " changes" << ": " << distNumChange[i] << " lines" << endl;

        tmp_changes += distNumChange[i];
        if (tmp_changes >= num_keep_lines_change){
          thd_change = i;
          cout << "threshold of change: " << thd_change << endl;
          break;
        }

      }
        
    }
    
    num_keep_lines_age = total_lines_age * THRESHOLD_PERCENT_CHANGES / 100;
    for (int j=0; j<DISTRIBUTION_AGE; j++){
      if (distNumAge[j] !=0){
        cout << j << " days" << ": " << distNumAge[j] << " lines" << endl;

        tmp_ages += distNumAge[j];
        if (tmp_ages >= num_keep_lines_age){
          thd_age = j;
          cout << "threshold of age: " << j << endl;
          break;
        }
      }
    }

    // get distribution for ranks
    num_keep_ranks = total_lines_age * THRESHOLD_PERCENT_CHANGES / 100;
    for (int k=0; k<DISTRIBUTION_RANK; k++){
      if (distNumRank[k] != 0){
        cout << k << " ranks" << ": " << distNumRank[k] << " lines" << endl;

        tmp_ranks += distNumRank[k];
        if (tmp_ranks >= num_keep_ranks){
          thd_rank = k;
          cout << "threshold of rank: " << k << endl;
          break;
        }
      }

    }

  } else{
    cout << "error: can't find working current working directory." << endl;
  }

  
}

