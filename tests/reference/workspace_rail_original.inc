// Frozen from 7aab01489e15d0a40d411d928a7ec7df5c663902 window.cpp:3596-3659.
// Host lookups are supplied by the independent test fixture.
    if (navigation_.rail.width>0) {
        Color rail=high_contrast_?background:ui.navigation;
        if (backdrop_) rail.a=240;
        rectangle(navigation_.rail,rail,0,{},0,true);
        rectangle({navigation_.rail.width-1/scale_,chrome_.band.height,1/scale_,navigation_.rail.height},seam);
        if (const auto amount=hover_amount(8); amount>0) rectangle(navigation_.toggle,opacity(raised,amount),5);
        const double tx=navigation_.toggle.x+16,ty=navigation_.toggle.y+14;
        rectangle({tx-7,ty-6,14,12},{0,0,0,0},1,secondary,1);
        line(tx-2,ty-6,tx-2,ty+6,secondary);
        if (navigation_.expanded) label({44,chrome_.band.height+10,132,18},"WORKSPACES",secondary,11);
        for (const auto& row:navigation_.projects) {
            const auto* project=projects_.find(row.project); if (!project) continue;
            const bool selected=row.project==projects_.active_id();
            const double amount=hover_amount((1ull<<62)|row.project.value);
            const double focus=workspace_focus_[row.project].value(now);
            if (focus>0||amount>0) rectangle(row.bounds,opacity(high_contrast_?raised:blend(raised,ui.selected,focus*.25),std::max(focus,amount*.65)),4);
            const auto icon=color_hex(project->metadata.accent,blend(secondary,accent,focus));
            const double x=row.bounds.x+10,y=row.bounds.y+10;
            if (project->metadata.icon=="folder") {
                rectangle({x,y+3,16,10},{0,0,0,0},1,icon,1);
                line(x,y+3,x,y,icon); line(x,y,x+6,y,icon); line(x+6,y,x+9,y+3,icon);
            } else if (project->metadata.icon=="code") {
                line(x+5,y+2,x,y+7,icon); line(x,y+7,x+5,y+12,icon);
                line(x+11,y+2,x+16,y+7,icon); line(x+16,y+7,x+11,y+12,icon);
            } else if (project->metadata.icon=="terminal") {
                rectangle({x,y,16,14},{0,0,0,0},2,icon,1);
                line(x+3,y+4,x+6,y+7,icon); line(x+6,y+7,x+3,y+10,icon); line(x+9,y+10,x+13,y+10,icon);
            } else if (project->metadata.icon=="bolt") {
                line(x+10,y,x+3,y+8,icon);line(x+3,y+8,x+9,y+8,icon);
                line(x+9,y+8,x+6,y+15,icon);line(x+6,y+15,x+14,y+5,icon);line(x+14,y+5,x+8,y+5,icon);
            } else if (project->metadata.icon=="globe") {
                rectangle({x,y,16,16},{0,0,0,0},8,icon,1);
                rectangle({x+5,y,6,16},{0,0,0,0},3,icon,1);line(x,y+8,x+16,y+8,icon);
            } else if (project->metadata.icon=="git") {
                line(x+4,y+3,x+4,y+14,icon);line(x+4,y+10,x+12,y+6,icon);line(x+12,y+6,x+12,y+3,icon);
                rectangle({x+2,y,4,4},{0,0,0,0},2,icon,1);rectangle({x+10,y,4,4},{0,0,0,0},2,icon,1);
                rectangle({x+2,y+12,4,4},{0,0,0,0},2,icon,1);
            } else if (project->metadata.icon=="flask") {
                line(x+5,y,x+11,y,icon);line(x+6,y,x+6,y+5,icon);line(x+10,y,x+10,y+5,icon);
                line(x+6,y+5,x+1,y+14,icon);line(x+10,y+5,x+15,y+14,icon);line(x+1,y+14,x+15,y+14,icon);
                line(x+4,y+9,x+12,y+9,icon);
            } else if (project->metadata.icon=="cloud") {
                rectangle({x,y+6,16,9},{0,0,0,0},4,icon,1);rectangle({x+4,y+1,9,11},{0,0,0,0},5,icon,1);
            } else {
                rectangle({x,y,16,15},{0,0,0,0},2,icon,1);
                if (project->metadata.icon=="book") {line(x+8,y,x+8,y+15,icon);line(x+3,y+4,x+6,y+4,icon);line(x+10,y+4,x+13,y+4,icon);}
                else {line(x,y+5,x+16,y+5,icon);line(x,y+10,x+16,y+10,icon);}
            }
            if (selected) rectangle({row.bounds.x+1,row.bounds.y+7,2,row.bounds.height-14},icon,1);
            if(!selected&&!navigation_.expanded&&project_watch_unread(project->id))rectangle({row.bounds.x+row.bounds.width-7,row.bounds.y+6,4,4},accent,2);
            if (navigation_.expanded) label({row.bounds.x+38,row.bounds.y+7,row.bounds.width-44,20},project->metadata.name+(!selected&&project_watch_unread(project->id)?" · result":""),blend(secondary,text,focus),13,selected);
        }
        if (const auto amount=hover_amount(9); amount>0) rectangle(navigation_.new_project,opacity(raised,amount),5);
        if (navigation_.new_project.height>0) {
            const double x=navigation_.new_project.x+18,y=navigation_.new_project.y+18;
            line(x-5,y,x+5,y,secondary); line(x,y-5,x,y+5,secondary);
            if (navigation_.expanded) label({navigation_.new_project.x+38,navigation_.new_project.y+8,navigation_.new_project.width-44,20},"New workspace",secondary,12);
        }
        if (navigation_.settings.height>0) {
            if (const auto amount=hover_amount(11); amount>0) rectangle(navigation_.settings,opacity(raised,amount),5);
            append_settings_icon(result,{navigation_.settings.x+18,navigation_.settings.y+18},8,secondary,static_cast<float>(scale_),1.2,z);
            if (navigation_.expanded) label({navigation_.settings.x+38,navigation_.settings.y+8,navigation_.settings.width-44,20},"Settings",secondary,12);
        }
    }
