  case SQLCOM_START_ALTER_TABLE:
  {
    /*
     Slave spawned start alter thread will not binlog, So we have to make sure
     that slave binlog will write flag FL_START_ALTER_E1
    */
    thd->transaction.start_alter= true;
    /*
     start_alter_thread will be true for spawned thread
    */
    if (thd->start_alter_thread)
    {
      res= lex->m_sql_cmd->execute(thd);
      break;
    }
    else if(!thd->rpt) //rpt should be NULL for legacy replication
    {
      /*
       We will just write the binlog and move to next event , because COMMIT
       Alter will take care of actual work
      */
      if (write_bin_log(thd, false, thd->query(), thd->query_length()))
        DBUG_RETURN(true);
      break;
    }
    pthread_t th;
    start_alter_thd_args *args= (start_alter_thd_args *) my_malloc(sizeof(
                                    start_alter_thd_args), MYF(0));
    args->rgi= thd->rgi_slave;
    args->query= {thd->query(), thd->query_length()};
    args->db= &thd->db;
    args->cs= thd->charset();
    args->catalog= thd->catalog;
    /*
     We could get shutdown at this moment so spawned thread just do the work
     till binlog writing of start alter and then exit.
    */
    args->shutdown= thd->rpt->stop;
    if (mysql_thread_create(key_rpl_parallel_thread, &th, &connection_attrib,
                            handle_slave_start_alter, args))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      goto error;
    }
    DBUG_ASSERT(thd->rgi_slave);
    Master_info *mi= thd->rgi_slave->rli->mi;
    start_alter_info *info=NULL;
    mysql_mutex_lock(&mi->start_alter_list_lock);
    List_iterator<start_alter_info> info_iterator(mi->start_alter_list);
    while(1)
    {
      while ((info= info_iterator++))
      {
        if(info->thread_id == thd->lex->previous_commit_id)
          break;
      }
      if (info && info->thread_id == thd->lex->previous_commit_id)
        break;
      mysql_cond_wait(&mi->start_alter_list_cond, &mi->start_alter_list_lock);
      info_iterator.rewind();
    }
    //Although write_start_alter can also remove the *info, so we can do this on any place
    if (thd->rpt->stop)
      info_iterator.remove();
    mysql_mutex_unlock(&mi->start_alter_list_lock);
    /*
     We can free the args here because spawned thread has already copied the data
    */
    my_free(args);
    DBUG_ASSERT(info->state == start_alter_state::REGISTERED);
    if (write_bin_log(thd, false, thd->query(), thd->query_length(), true) && ha_commit_trans(thd, true))
      return true;
    thd->transaction.start_alter= false;
    break;
  }

  case SQLCOM_COMMIT_ALTER:
  {
    DBUG_ASSERT(thd->rgi_slave);
    Master_info *mi= thd->rgi_slave->rli->mi;
    start_alter_info *info=NULL;
    mysql_mutex_lock(&mi->start_alter_list_lock);
    List_iterator<start_alter_info> info_iterator(mi->start_alter_list);
    while ((info= info_iterator++))
    {
      if(info->thread_id == thd->lex->previous_commit_id)
      {
        info_iterator.remove();
        break;
      }
    }
    mysql_mutex_unlock(&mi->start_alter_list_lock);
    if (!info || info->thread_id != thd->lex->previous_commit_id)
    {
      //error handeling
      DBUG_ASSERT(lex->m_sql_cmd != NULL);
      //direct_commit_alter is used so that mysql_alter_table should not do
      //unnecessary binlogging or spawn new thread because there is no start
      //alter context
      thd->direct_commit_alter= true;
      res= lex->m_sql_cmd->execute(thd);
      thd->direct_commit_alter= false;
      DBUG_PRINT("result", ("res: %d  killed: %d  is_error: %d",
                          res, thd->killed, thd->is_error()));
      if (write_bin_log(thd, true, thd->query(), thd->query_length()))
        DBUG_RETURN(true);
      break;
    }
    /*
     start_alter_state can be either ::REGISTERED or ::WAITING
     */
    mysql_mutex_lock(&mi->start_alter_lock);
    while(info->state == start_alter_state::REGISTERED )
      mysql_cond_wait(&mi->start_alter_cond, &mi->start_alter_lock);
    mysql_mutex_unlock(&mi->start_alter_lock);
    mysql_mutex_lock(&mi->start_alter_lock);
    info->state= start_alter_state::COMMIT_ALTER;
    mysql_mutex_unlock(&mi->start_alter_lock);
    mysql_cond_broadcast(&mi->start_alter_cond);
    // Wait for commit by worker thread
    mysql_mutex_lock(&mi->start_alter_lock);
    while(info->state != start_alter_state::COMMITTED )
      mysql_cond_wait(&mi->start_alter_cond, &mi->start_alter_lock);
    mysql_mutex_unlock(&mi->start_alter_lock);
    my_free(info);
    if (write_bin_log(thd, true, thd->query(), thd->query_length()))
      DBUG_RETURN(true);
    break;
  }

  case SQLCOM_ROLLBACK_ALTER:
  {
    DBUG_ASSERT(thd->rgi_slave);
    Master_info *mi= thd->rgi_slave->rli->mi;
    start_alter_info *info=NULL;
    mysql_mutex_lock(&mi->start_alter_list_lock);
    List_iterator<start_alter_info> info_iterator(mi->start_alter_list);
    while ((info= info_iterator++))
    {
      if(info->thread_id == thd->lex->previous_commit_id)
      {
        info_iterator.remove();
        break;
      }
    }
    mysql_mutex_unlock(&mi->start_alter_list_lock);
    if (!info || info->thread_id != thd->lex->previous_commit_id)
    {
      //error handeling
      DBUG_ASSERT(lex->m_sql_cmd != NULL);
      //Just write the binlog because there is nothing to be done
      if (write_bin_log(thd, true, thd->query(), thd->query_length()))
        DBUG_RETURN(true);
      break;
    }
    /*
     start_alter_state can be either ::REGISTERED or ::WAITING
     */
    mysql_mutex_lock(&mi->start_alter_lock);
    while(info->state == start_alter_state::REGISTERED )
      mysql_cond_wait(&mi->start_alter_cond, &mi->start_alter_lock);
    mysql_mutex_unlock(&mi->start_alter_lock);
    mysql_mutex_lock(&mi->start_alter_lock);
    info->state= start_alter_state::ROLLBACK_ALTER;
    mysql_mutex_unlock(&mi->start_alter_lock);
    mysql_cond_broadcast(&mi->start_alter_cond);
    // Wait for commit by worker thread
    mysql_mutex_lock(&mi->start_alter_lock);
    while(info->state != start_alter_state::COMMITTED )
      mysql_cond_wait(&mi->start_alter_cond, &mi->start_alter_lock);
    mysql_mutex_unlock(&mi->start_alter_lock);
    my_free(info);
    if (write_bin_log(thd, true, thd->query(), thd->query_length()))
      DBUG_RETURN(true);
    break;
  }
