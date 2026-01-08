#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ncurses.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#define MAX_PROCESSES 1024
#define BAR_WIDTH 16

typedef struct {
    int pid;
    char name[64];
    char cmdline[256];
    char state;
} Process;

Process processes[MAX_PROCESSES];
int total_processes = 0;
int zombie_count = 0;

// CPU metrics
unsigned long long prev_user = 0, prev_nice = 0, prev_system = 0, prev_idle = 0;

// Read CPU usage
float get_cpu_usage() {
    FILE *fp = fopen("/proc/stat","r");
    if(!fp) return 0;
    unsigned long long user, nice, system, idle;
    fscanf(fp,"cpu %llu %llu %llu %llu",&user,&nice,&system,&idle);
    fclose(fp);
    unsigned long long d_user = user - prev_user;
    unsigned long long d_nice = nice - prev_nice;
    unsigned long long d_system = system - prev_system;
    unsigned long long d_idle = idle - prev_idle;
    prev_user = user; prev_nice = nice; prev_system = system; prev_idle = idle;
    unsigned long long total = d_user + d_nice + d_system + d_idle;
    if(total == 0) return 0;
    return (float)(d_user + d_nice + d_system) * 100 / total;
}

// Read RAM usage
float get_ram_usage() {
    FILE *fp = fopen("/proc/meminfo","r");
    if(!fp) return 0;
    unsigned long mem_total = 0, mem_available = 0;
    char line[128];
    while(fgets(line,sizeof(line),fp)) {
        if(sscanf(line,"MemTotal: %lu kB",&mem_total)==1) continue;
        if(sscanf(line,"MemAvailable: %lu kB",&mem_available)==1) break;
    }
    fclose(fp);
    if(mem_total==0) return 0;
    return (float)(mem_total - mem_available) * 100 / mem_total;
}

// Read disk usage for root
float get_disk_usage() {
    struct statvfs fs;
    if(statvfs("/",&fs)!=0) return 0;
    unsigned long total = fs.f_blocks;
    unsigned long free = fs.f_bfree;
    if(total==0) return 0;
    return (float)(total - free) * 100 / total;
}

// Read processes from /proc
void read_processes() {
    DIR *dir = opendir("/proc");
    struct dirent *entry;
    total_processes = 0;
    zombie_count = 0;
    while((entry=readdir(dir))) {
        int pid = atoi(entry->d_name);
        if(pid<=0) continue;
        if(total_processes>=MAX_PROCESSES) break;
        char path[128], state;
        sprintf(path,"/proc/%d/stat",pid);
        FILE *fp = fopen(path,"r");
        if(!fp) continue;
        char comm[64];
        fscanf(fp,"%d %63s %c",&pid,comm,&state);
        fclose(fp);

        // Count zombies
        if(state=='Z') zombie_count++;

        // Remove parentheses from process name
        size_t len = strlen(comm);
        if(len>2 && comm[0]=='(' && comm[len-1]==')') {
            comm[len-1] = '\0';
            memmove(comm, comm+1, len-1);
        }

        processes[total_processes].pid = pid;
        strcpy(processes[total_processes].name,comm);
        processes[total_processes].state = state;

        // read cmdline
        sprintf(path,"/proc/%d/cmdline",pid);
        fp = fopen(path,"r");
        if(fp) {
            size_t n = fread(processes[total_processes].cmdline,1,255,fp);
            processes[total_processes].cmdline[n]='\0';
            fclose(fp);
        } else {
            strcpy(processes[total_processes].cmdline,"");
        }
        total_processes++;
    }
    closedir(dir);
}

// Draw a horizontal bar
void draw_bar(int y,int x,const char* label,float percent) {
    mvprintw(y,x,"%s [",label);
    int fill = (int)(percent * BAR_WIDTH / 100);
    for(int i=0;i<BAR_WIDTH;i++) {
        if(i<fill) addch('=');
        else addch(' ');
    }
    printw("] %.0f%%",percent);
}

// Show details window for a process with RGB(166,0,0)
void show_details(Process *p) {
    int h=7,w=50,y=(LINES-h)/2,x=(COLS-w)/2;
    WINDOW *win = newwin(h,w,y,x);
    wattron(win,COLOR_PAIR(5)); // custom red
    box(win,0,0);
    mvwprintw(win,1,2,"PID: %d",p->pid);
    mvwprintw(win,2,2,"Name: %s",p->name);
    mvwprintw(win,3,2,"Cmd: %s",p->cmdline[0]?p->cmdline:"(none)");
    mvwprintw(win,5,2,"Press any key to close");
    wattroff(win,COLOR_PAIR(5));
    wrefresh(win);
    getch();
    delwin(win);
}

// Main function
int main() {
    initscr();
    cbreak();
    noecho();
    keypad(stdscr,TRUE);
    timeout(1000); // 1s refresh
    start_color();
    use_default_colors();
    init_pair(1,COLOR_GREEN,-1); // CPU,RAM,DISKS,ZOMBIES
    init_pair(2,COLOR_CYAN,-1);  // selected process
    init_pair(3,COLOR_BLUE,-1);  // process list
    init_pair(4,COLOR_RED,-1);   // fallback
    init_color(10,650,0,0);      // custom red ~ RGB(166,0,0)
    init_pair(5,10,-1);          // process info window

    int selected = 0;
    int start_index = 0;

    while(1) {
        clear();
        float cpu=get_cpu_usage();
        float ram=get_ram_usage();
        float disk=get_disk_usage();

        // draw bars
        attron(COLOR_PAIR(1));
        draw_bar(0,0,"CPU",cpu);
        draw_bar(1,0,"RAM",ram);
        draw_bar(2,0,"DISKS",disk);
        mvprintw(3,0,"ZOMBIES: %d",zombie_count); // green
        attroff(COLOR_PAIR(1));

        // add an empty line after ZOMBIES
        mvprintw(4,0,"");

        read_processes();

        int max_rows = LINES-6; // adjust for bars + zombies + empty line
        if(selected>=total_processes) selected = total_processes-1;
        if(selected<0) selected=0;
        if(start_index>selected) start_index=selected;
        if(selected>=start_index+max_rows) start_index=selected-max_rows+1;

        // draw process list with scroll
        for(int i=0;i<max_rows && i+start_index<total_processes;i++) {
            int idx = i+start_index;
            if(idx==selected) attron(COLOR_PAIR(2));
            else attron(COLOR_PAIR(3));
            mvprintw(i+5,0,"%5d %-15s",processes[idx].pid,processes[idx].name);
            attroff(COLOR_PAIR(2));
            attroff(COLOR_PAIR(3));
        }

        mvprintw(LINES-1,0,"Press ENTER for details, q to quit");

        refresh();

        int ch = getch();
        if(ch=='q') break;
        else if(ch==KEY_UP) {
            if(selected>0) selected--;
        } else if(ch==KEY_DOWN) {
            if(selected<total_processes-1) selected++;
        } else if(ch=='\n') {
            show_details(&processes[selected]);
        }
    }

    endwin();
    return 0;
}
