
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
#include <vector>


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


/* if "fullStr" ends with "ending", return true.*/
bool endsWith(std::string fullStr, std::string ending){
  if (fullStr.length() >= ending.length()){
    if (fullStr.compare(fullStr.length() - ending.length(), ending.length(), ending) == 0) return true;
    else return false;
  } else return false;

}

bool startsWith(std::string big_str, std::string small_str){
  if (big_str.compare(0, small_str.length(), small_str) == 0) return true;
  else return false;
}

// if both arrays are @@, return true.
bool isATAT(char head_atat[], char tail_atat[]){
  std::string str_head_atat, str_tail_atat;
  str_head_atat.assign(head_atat);
  str_tail_atat.assign(tail_atat);

  if (str_head_atat.compare("@@")!=0 
        || str_tail_atat.compare("@@")!=0) return false;

  return true;
}


/* use popen() to execute git command */
std::string execute_git_cmd (std::string directory, std::string str_cmd){
  FILE *fp;
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
    str_res.assign(ch_git_res);
  }

  if (startsWith(str_res, "fatal")){
    str_res = "";
  }

  pclose(fp);

  return str_res;

}


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


/* if same, return true */
bool isCommitsSame(std::string cmt1, std::string cmt2){
  
  if (cmt1.length() <= cmt2.length()){
    if (cmt1.compare(0, cmt1.length(), 
          cmt2, 0, cmt1.length()) == 0){
      return true;
    }    
  } else{
    if (cmt2.compare(0, cmt2.length(), 
          cmt1, 0, cmt2.length()) == 0){
      return true;
    }     
  }

  return false;
}

/* Check if file exists in HEAD using command mode.
return:
    exist: 1; not exist: 0 */
bool is_file_exist(std::string relative_file_path, std::string git_directory,
                    std::string commit_sha){

  //string cmd("cd /home/usrname/repo && git cat-file -e HEAD:util/read.h 2>&1");
  std::ostringstream cmd;

  char result_buf[1024];
  bool isSuccess = false;
  FILE *fp;

  if(access(git_directory.c_str(), F_OK) == -1) return false;
  
  cmd << "cd " << git_directory << " && git cat-file -e "
      << commit_sha << ":"
      << relative_file_path << " 2>&1";

	fp = popen(cmd.str().c_str(), "r");
	if(NULL == fp) return false;
	// when cmd fail, output "fatal: Not a valid object name ...";
  // when succeed, output nothing
  if (fgets(result_buf, sizeof(result_buf), fp) != NULL) isSuccess = false;
  else isSuccess = true;
	
  pclose(fp);
  
  return isSuccess;

}


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

  pclose(fp);

}

// get unix time of a commit
unsigned long cmt_unix_time(std::string git_directory, std::string cmt_sha){
    FILE *unixfp;
    unsigned long utime = 0;
    std::ostringstream unix_cmd;
    // result: 1557509398
    unix_cmd << "cd " << git_directory 
            << " && git show --no-patch --no-notes --pretty='%at' " 
            << cmt_sha;
    unixfp = popen(unix_cmd.str().c_str(), "r");
    if (NULL== unixfp) return 0;
    if (fscanf(unixfp, "%lu", &utime) != 1) utime = 0;
    pclose(unixfp);

    return utime;

}


/* git diff parent HEAD */
/* git diff, get changed lines in current commit.
    Find the changed line numbers in file relative_file_path as it was changed in HEAD, 
    and add them to the list changed_lines_cur_commit     
 */
void git_diff_parent_head(std::string git_directory, std::string relative_file_path, 
              std::string parent_commit, std::set<unsigned int> &changed_lines_cur_commit){

    std::ostringstream cmd;
    
    std::string current_line_range;
    FILE *fp;
    int num_start, num_count;

    // git show: parent_commit(-) current_commit(+)
    // result: "@@ -8,0 +9,2 @@" or "@@ -10 +11,0 @@" or "@@ -466,8 +475 @@" or "@@ -8 +9 @@"
    // output: "9,2" or "11,0" (the lines are deleted) or "8"
    cmd << "cd " << git_directory 
        << " && git diff --oneline -U0 " 
        << parent_commit << " HEAD " 
        << " -- " << relative_file_path
        << " | grep -o -P \"^@@ -[0-9]+(,[0-9]+)? \\+\\K[0-9]+(,[0-9]+)?(?= @@)\"";

    fp = popen(cmd.str().c_str(), "r");
    if(NULL == fp) return;
    // get numbers in (+): current commit
    num_count = -1; // the result shows no count
    while(fscanf(fp, "%d,%d", &num_start, &num_count) >= 1){
      if (num_count == -1){ // result: 8
        changed_lines_cur_commit.insert(num_start);
      } else if (num_count > 0){ // result: 9,2
        for (int i = 0; i < num_count; i++){
          changed_lines_cur_commit.insert(num_start + i);
        }
      }

      num_count = -1;
    }

   pclose(fp);
}

/* Get changes in HEAD commit */

void getHeadChanges(std::string git_directory, std::string relative_file_path, 
                                std::set<unsigned int> &changed_lines_num){
    std::string commit_sha;
    char res_commit[SHA_LENGTH];
    FILE *fp;
    std::ostringstream pc_cmd;

    // get parent commits of HEAD
    // succeeds: output parent commit sha; fail: output "fatal: ..."
    pc_cmd << "cd " << git_directory << " && "
           << "git show -s --pretty=%P HEAD 2>&1";
    fp = popen(pc_cmd.str().c_str(), "r");
    if (NULL == fp) return;
    while (fscanf(fp, "%s", res_commit) == 1){
      commit_sha.assign(res_commit);
      if (commit_sha.empty()) break;
      if (startsWith(commit_sha, "fatal")) break;

      git_diff_parent_head(git_directory, relative_file_path, 
                                  commit_sha, changed_lines_num);
    }

    pclose(fp);

}

/* get 'git blame' in HEAD commit */
  /* largest unix time => SHA of commit with the largest unix time => line pair 
  //unix time, blame commit sha
  map<unsigned long, string> utime2commit; 
  // SHA of blame commit, (blame line number, head line number)
  map<string, map<unsigned int, unsigned int>> commit2Blame2HeadLine; */
void createHeadRecords(std::string relative_file_path, std::string git_directory,
              std::map<unsigned long, std::string> &utime2commit, 
              std::map<std::string, std::map<unsigned int, unsigned int>> &commit2Blame2HeadLine){
    /* git blame -p $commit_SHA  -- relative_file_path
       | grep -Po "^[0-9a-f]+ [0-9]+ [0-9]+" */
  /* result: output: commit_hash old_line_num current_line_num
                      7db7d5b73368 12 31 */

  char blame_commit[SHA_LENGTH] = {0};
  unsigned int head_line, blame_line;
  unsigned long utime;
  FILE *headfp;

  std::string str_blame_commit;
  std::map<std::string, unsigned long> cmt2time;

  std::ostringstream headblame_cmd;
  headblame_cmd << "cd " << git_directory
            << " && git blame -p HEAD -- " << relative_file_path
            << " | grep -Po \"^[0-9a-f]+ [0-9]+ [0-9]+\"";
  headfp = popen(headblame_cmd.str().c_str(), "r");
  if (NULL == headfp) return;

  while (fscanf(headfp,"%s %u %u", blame_commit, &blame_line, &head_line) == 3){
    str_blame_commit.assign(blame_commit);
    if (str_blame_commit.empty()) continue;

    if (!cmt2time.count(str_blame_commit)){
      utime = cmt_unix_time(git_directory, str_blame_commit);
      cmt2time[str_blame_commit] = utime;
      utime2commit[utime] = str_blame_commit;
    } else{
      utime2commit[cmt2time[str_blame_commit]] = str_blame_commit;
    }

    commit2Blame2HeadLine[str_blame_commit][blame_line] = head_line;
    
  }

  pclose(headfp);
      

}

/* Count lines in the latest commit. */
void countInLatestCommit(std::string relative_file_path, std::string git_directory,
              std::map<unsigned long, std::string> &utime2commit,
              unsigned long utime_largest, 
              std::map<std::string, std::map<unsigned int, unsigned int>> &commit2Blame2HeadLine,
              std::map<unsigned int, unsigned int> &headline2count){

  char blamed_commit[SHA_LENGTH] = {0};
  unsigned int cur_line, blame_line, head_line;
  unsigned long utime;
  FILE *headfp;

  std::string str_blame_commit, cur_commit;
  std::map<std::string, unsigned long> cmt2time;
  std::ostringstream headblame_cmd;

  cur_commit = utime2commit[utime_largest];

  /* Get the lines touched in the cur_commit: previous blamed line, head line;
      In the current commit, the previous blamed line == the current line */
  auto cur2headLines = commit2Blame2HeadLine[cur_commit];
  auto curline_min = cur2headLines.begin()->first;
  auto curline_max = cur2headLines.rbegin()->first;

  // check if file exists in current commit. TODO: renamed file?
  if (!is_file_exist(relative_file_path, git_directory, cur_commit)) return;

  /* git blame -p -L 10,30 1485aacb161d72  -- meson.build 
        | grep -Po "^[0-9a-f]+ [0-9]+ [0-9]+" */
  /* output: blame_commit_hash blame_line_num current_line_num
                      7db7d5b73368 12 31 */
  headblame_cmd << "cd " << git_directory
            << " && git blame -p "    // -p: for the "grep -Po"
            << " -L "<< curline_min << "," << curline_max << " "  // line range
            << cur_commit  // the current commit
            << " --ignore-rev " << cur_commit // Don't blame itself!!!
            << " -- " << relative_file_path
            << " | grep -Po \"^[0-9a-f]+ [0-9]+ [0-9]+\"";
  headfp = popen(headblame_cmd.str().c_str(), "r");
  if (NULL == headfp) return;
  // head line => cur_line => blame line
  while (fscanf(headfp, "%s %u %u", blamed_commit, &blame_line, &cur_line) == 3){

    str_blame_commit.assign(blamed_commit);
    if (str_blame_commit.empty()) continue;
    // only deal with the lines touched in the current commit (i.e., previous blamed commit)
    if (!cur2headLines.count(cur_line)) continue;

    /* If the blamed commit equals to the current commit, 
          the current commit is the one that creates the lines */
    if (isCommitsSame(str_blame_commit, cur_commit)) continue;
    
    // don't repeatedly compute the unix time of the same commit
    if (!cmt2time.count(str_blame_commit)){
      utime = cmt_unix_time(git_directory, str_blame_commit);
      cmt2time[str_blame_commit] = utime;
      utime2commit[utime] = str_blame_commit;
    } else{
      utime2commit[cmt2time[str_blame_commit]] = str_blame_commit;
    }

    head_line = cur2headLines[cur_line];
    commit2Blame2HeadLine[str_blame_commit][blame_line] = head_line;

    /* Finally!! Count the #changes */
    if (headline2count.count(head_line))
        headline2count[head_line]++;
    else
        headline2count[head_line] = 1;

  }

  pclose(headfp);

}

/* get change distribution using git blame */
void get_change_dist_git_blame(std::string relative_file_path, std::string git_directory){
  unsigned long utime_largest;
  std::string cur_commit;
   /* largest unix time => SHA of commit with the largest unix time => line pair */
  // unix time, blamed commit sha
  std::map<unsigned long, std::string> utime2commit;
  // SHA of blame commit, (line number of blame commit, head line number)
  std::map<std::string, std::map<unsigned int, unsigned int>> commit2Blame2HeadLine;
  // head line, count of #changes
  std::map<unsigned int, unsigned int> headline2count;

  /* get 'git blame' in HEAD commit */
  createHeadRecords(relative_file_path, git_directory, 
                          utime2commit, commit2Blame2HeadLine);

  /* Get the #changes of lines.
    We travel backward time so that commits will not be "git blame"d repeatedly. 
    utime_largest: the unix time of the most recent blamed commit */
  while(!utime2commit.empty()){
    // get the commit with the largest unix time
    auto largest_utime_it = utime2commit.rbegin();
    utime_largest = largest_utime_it->first;
    cur_commit = largest_utime_it->second;

    countInLatestCommit(relative_file_path, git_directory, utime2commit, utime_largest, 
                              commit2Blame2HeadLine, headline2count);

    /* remove the record of the analyzed commit */
    commit2Blame2HeadLine.erase(cur_commit);
    utime2commit.erase(utime_largest);
  }

  // Changes in HEAD
  std::set<unsigned int> changed_lines_num;
  getHeadChanges(git_directory, relative_file_path, changed_lines_num);
  if (!changed_lines_num.empty()){
    for (auto headcn : changed_lines_num){
      if (headline2count.count(headcn)) headline2count[headcn]++;
      else headline2count[headcn] = 1;
    }
  }

  
  // record #changes
  for (auto hc : headline2count){
    if (hc.second < DISTRIBUTION_CHANGE){
      distNumChange[hc.second]++;
      total_lines_changes++;
    } else{
      distNumChange[DISTRIBUTION_CHANGE - 1]++;
      total_lines_changes++;
    }
      
  }

}


/* White list for counting ages/#changes of C/C++ files. 
  TODO: more to add. */
bool isCountWhiteSuffix(std::string filename){
  std::vector<std::string> whiteSuffix{".c", ".C", ".cc", ".CC", ".cpp", 
            ".CPP", ".c++", ".cp", ".cxx", ".CXX", 
            ".h", ".H", ".hpp", ".HPP", ".hh", ".hxx", ".h++"};
  for (auto ws : whiteSuffix){
    if (endsWith(filename, ws)) return true;
  }

  return false;
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

      abs_file_path = abs_dir + "/" + fname;
      stat(abs_file_path.c_str(), &filestat);
      if (S_ISDIR(filestat.st_mode)){ // directory
        listFilesGetDistribution(git_dir, cur_rela_path, head_commit_days, max_num_ranks);

      } else {// file
        
        if (!isCountWhiteSuffix(fname)) continue;

        // file not in HEAD commit
        if (!is_file_exist(cur_rela_path, git_dir, "HEAD")) continue;

        // std::cout << abs_file_path << endl;
        /* Get distribution of #changes */
        get_change_dist_git_blame(cur_rela_path, git_dir);
        
        // get_line_change_dist(cur_rela_path, git_dir);

        /* Get distribution of age */
        get_line_age_dist(cur_rela_path, git_dir, head_commit_days);

        // /* Get distribution of #ranks */
        get_line_rank_dist(cur_rela_path, git_dir, max_num_ranks);

      }

      
    }
  }

  closedir(dir);

  return;
}

// false: recording files don't exist
bool CheckandReadFiles(std::string git_dir){
  unsigned int record_total_lines_age = 0, record_total_lines_change = 0;
  std::ifstream dist_age_file (git_dir + "/" + DIST_AGES_FILE);
  std::ifstream dist_ranks_file (git_dir + "/" + DIST_RANKS_FILE);
  std::ifstream dist_change_file (git_dir + "/" + DIST_CHANGES_FILE);
  if (!dist_age_file.good() 
        || !dist_ranks_file.good() 
        || !dist_change_file.good()) return false;

  int index, lines_count, file_line;
  string line;

  if (dist_age_file.is_open()){
    file_line = 0;
    while(std::getline(dist_age_file, line)){
      if (file_line != 0){
        std::istringstream ssline(line);
        ssline >> index >> lines_count;
        // cout << index << " age(days): " << lines_count << " lines" << endl;
        distNumAge[index] = lines_count;
        record_total_lines_age += lines_count;
      }

      file_line++;
    }
    dist_age_file.close();
    total_lines_age = record_total_lines_age;
  }

  if (dist_ranks_file.is_open()){
    file_line = 0;
    while(std::getline(dist_ranks_file, line)){
      if (file_line != 0){
        std::istringstream ssline(line);
        ssline >> index >> lines_count;
        // cout << index << " #ranks: " << lines_count << " lines"<< endl;
        distNumRank[index] = lines_count;
      }

      file_line++;
    }
    dist_ranks_file.close();
  }

  if (dist_change_file.is_open()){
    file_line = 0;
    while(std::getline(dist_change_file, line)){
      if (file_line != 0){
        std::istringstream ssline(line);
        ssline >> index >> lines_count;
        // cout << index << " #change: " << lines_count << " lines"<< endl;
        distNumChange[index] = lines_count;
        record_total_lines_change += lines_count;
      }

      file_line++;
    }
    dist_change_file.close();
    total_lines_changes = record_total_lines_change;
  }

  return true;
}

/* Display usage hints. */

static void usage(char* argv0) {

  SAYF("\n%s [ options ] \n\n"

       "Required Parameters:\n\n"

       "  -d dir        - directory of the target program.\n\n"

       "Others:\n\n"

       "  -p integer    - N%% for BBs that always insert churn info;\n"
       "                  The default is set to %d.\n"
       "  -h            - show this usage hint\n\n",

       argv0, ALWAYS_INSERT_PERCENT);

  exit(1);

}

int main(int argc, char **argv){

  s32 opt;
  char* prog_dir = NULL;
  unsigned int per_keep = ALWAYS_INSERT_PERCENT; // percentage for always insertion
  std::string str_cur_workp;
  struct dirent *dp;
  int head_commit_days, // head commit in unix time, days
      max_num_ranks;   // #commits before HEAD
  int num_keep_lines_age, num_keep_ranks, num_keep_lines_change;
  int tmp_ages = 0, tmp_changes = 0, tmp_ranks = 0;
  bool is_thrd_set = false, isRecorded = false;
  std::ofstream dist_age_file, dist_ranks_file, dist_change_file, dist_threshold_file;

  while ((opt = getopt(argc, argv, "d:p:h")) > 0){
    switch (opt){
      case 'd':
        prog_dir = optarg;
        break;

      case 'p':
        if (sscanf(optarg, "%u", &per_keep) < 1 ||
            optarg[0] == '-' || per_keep > 100 || per_keep == 0) 
                FATAL("Bad syntax for -p");

        break;

      case 'h':
      default:
        usage(argv[0]);
    }
  }

  if (!prog_dir)  usage(argv[0]);

  str_cur_workp.assign(prog_dir);

  std::string cmd_repo ("git rev-parse --show-toplevel");
  std::string git_path = execute_git_cmd(str_cur_workp, cmd_repo);

  if (git_path.empty()) {
    FATAL("Error: can't find git directory.");
    return 0;
  }

  dist_threshold_file.open(git_path + "/" + DIST_THRESHOLD_FILE, std::ios::trunc);
  if(!dist_threshold_file.is_open()){
    FATAL("Cannot create file for recording thresholds info.");
  }
  dist_threshold_file << "Percentage of lines to insert churn info: " << per_keep << "%" << endl;

  OKF("The distribution files are in %s", git_path.c_str());
  OKF("Percentage of lines to insert churn info: %u%%", per_keep);
  
  isRecorded = CheckandReadFiles(git_path);

  if (!isRecorded){
    /* files for recording distribution */
    dist_age_file.open(git_path + "/" + DIST_AGES_FILE);
    dist_ranks_file.open(git_path + "/" + DIST_RANKS_FILE);
    dist_change_file.open(git_path + "/" + DIST_CHANGES_FILE);

    if(!dist_age_file.is_open() || !dist_ranks_file.is_open() ||
          !dist_change_file.is_open()){
      FATAL("Cannot create files for recording distribution info.");
    }
        
    // get distribution for age and churn
    std::string head_cmd("git show -s --format=%ct HEAD");
    head_commit_days = get_commit_time_days(git_path, head_cmd);
    max_num_ranks = get_max_ranks(git_path);
    if (max_num_ranks == WRONG_VALUE || head_commit_days == WRONG_VALUE)
      FATAL("Cannot get the max #ranks and/or max days");

    listFilesGetDistribution(git_path, "", head_commit_days, max_num_ranks);

  }

  // cout << "total chagnes: " << total_lines_changes << endl;
  num_keep_lines_change = total_lines_changes * per_keep / 100;
  is_thrd_set = false;
  if(!isRecorded) dist_change_file << "#changes  lines_count" << endl;
  for (int i = DISTRIBUTION_CHANGE - 1; i > 0 ; i--){
    if (distNumChange[i] != 0) {
      if(!isRecorded) dist_change_file << i << "  " << distNumChange[i] << endl;
      tmp_changes += distNumChange[i];
      if (tmp_changes >= num_keep_lines_change && !is_thrd_set){
        OKF("Threshold of #change: %d", i);
        dist_threshold_file << "Threshold of #change: " << i << endl;
        is_thrd_set = true;
        if(isRecorded) break;
      }
    }     
  }
  
  // num_keep_lines_age = total_lines_age * per_keep / 100;
  num_keep_lines_age = total_lines_changes * per_keep / 100;
  is_thrd_set = false;
  if(!isRecorded) dist_age_file << "age(days)  lines_count" <<endl;
  for (int j=0; j<DISTRIBUTION_AGE; j++){
    if (distNumAge[j] !=0){
      if(!isRecorded) dist_age_file<< j <<"  "<< distNumAge[j] <<endl;
      tmp_ages += distNumAge[j];
      if (tmp_ages >= num_keep_lines_age  && !is_thrd_set){
        OKF("Threshold of age(days): %d", j);
        dist_threshold_file << "Threshold of age(days): " << j << endl;
        is_thrd_set = true;
        if(isRecorded) break;
      }
    }
  }

  // get distribution for ranks
  // num_keep_ranks = total_lines_age * per_keep / 100;
  num_keep_ranks = total_lines_changes * per_keep / 100;
  is_thrd_set = false;
  if(!isRecorded) dist_ranks_file << "#ranks  lines_count" << endl;
  for (int k=0; k<DISTRIBUTION_RANK; k++){
    if (distNumRank[k] != 0){
      if(!isRecorded) dist_ranks_file << k << "  " <<  distNumRank[k] << endl;
      tmp_ranks += distNumRank[k];
      if (tmp_ranks >= num_keep_ranks  && !is_thrd_set){
        OKF("Threshold of #ranks: %d", k);
        dist_threshold_file << "Threshold of #rank: " << k << endl;
        is_thrd_set = true;
        if(isRecorded) break;
      }
    }
  }

  if(!isRecorded){
    dist_age_file.close();
    dist_ranks_file.close();
    dist_change_file.close();
  }

  dist_threshold_file.close();


}

