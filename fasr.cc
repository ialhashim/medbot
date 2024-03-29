#include "stage.hh"

using namespace Stg;

const bool verbose = false;

// navigation control params
const double cruisespeed = 0.4;
const double avoidspeed = 0.05;
const double avoidturn = 0.5;
const double minfrontdistance = 0.7;
const double stopdist = 0.5;
const int avoidduration = 10;
const int workduration = 20;
const int payload = 1;

float death_level = 0.05;
float hunger_level = 0.25;

double have[4][4] =
{
    //  { -120, -180, 180, 180 },
    //{ -90, -120, 180, 90 },
    { 90, 180, 180, 180 },
    { 90, -90, 180, 90 },
    { 90, 90, 180, 90 },
    { 0, 45, 0, 0}
};

double need[4][4] =
{
    { -120, -180, 180, 180 },
    { -90, -120, 180, 90 },
    { -90, -90, 180, 180 },
    { -90, -180, -90, -90 }
};

double refuel[4][4] =
{
    {  0, 0, 45, 120 },
    { 0,-90, -60, -160 },
    { -90, -90, 180, 180 },
    { -90, -180, -90, -90 }
};

typedef enum
{
    MODE_WORK=0,
    MODE_DOCK,
    MODE_UNDOCK,
    MODE_DEAD
} nav_mode_t;

class Robot
{
private:
    ModelPosition* pos;
    ModelLaser* laser;
    ModelRanger* ranger;
    ModelFiducial* fiducial;
    ModelBlobfinder* blobfinder;
    ModelGripper* gripper;
    Model *source, *sink;
    int avoidcount, randcount;
    int work_get, work_put;
    bool charger_ahoy;
    double charger_bearing;
    double charger_range;
    double charger_heading;
    nav_mode_t mode;
    bool at_dest;

public:

    Robot( ModelPosition* pos,
           Model* source,
           Model* sink )
            : pos(pos),
            laser( (ModelLaser*)pos->GetUnusedModelOfType( MODEL_TYPE_LASER )),
            ranger( (ModelRanger*)pos->GetUnusedModelOfType( MODEL_TYPE_RANGER )),
            fiducial( (ModelFiducial*)pos->GetUnusedModelOfType( MODEL_TYPE_FIDUCIAL )),
            blobfinder( (ModelBlobfinder*)pos->GetUnusedModelOfType( MODEL_TYPE_BLOBFINDER )),
            gripper( (ModelGripper*)pos->GetUnusedModelOfType( MODEL_TYPE_GRIPPER )),
            source(source),
            sink(sink),
            avoidcount(0),
            randcount(0),
            work_get(0),
            work_put(0),
            charger_ahoy(false),
            charger_bearing(0),
            charger_range(0),
            charger_heading(0),
            mode(MODE_WORK),
            at_dest( false )
    {
        // need at least these models to get any work done
        // (pos must be good, as we used it in the initialization list)
        assert( laser );
        assert( source );
        assert( sink );

        // PositionUpdate() checks to see if we reached source or sink
        pos->AddUpdateCallback( (stg_model_callback_t)PositionUpdate, this );
        pos->Subscribe();

        // LaserUpdate() controls the robot, by reading from laser and
        // writing to position
        laser->AddUpdateCallback( (stg_model_callback_t)LaserUpdate, this );
        laser->Subscribe();

        fiducial->AddUpdateCallback( (stg_model_callback_t)FiducialUpdate, this );
        fiducial->Subscribe();

        //gripper->AddUpdateCallback( (stg_model_callback_t)GripperUpdate, this );
        gripper->Subscribe();

        if ( blobfinder ) // optional
        {
            blobfinder->AddUpdateCallback( (stg_model_callback_t)BlobFinderUpdate, this );
            blobfinder->Subscribe();
        }

        // Insert robots into a table
        GHashTable* robots_table = NULL;
        if(!(robots_table = (GHashTable*) pos->GetWorld()->GetModel("cave")->GetProperty("robots_table"))){
            robots_table = g_hash_table_new( g_direct_hash, g_direct_equal );
            pos->GetWorld()->GetModel("cave")->SetProperty("robots_table", (void*) robots_table);
        }

        g_hash_table_insert( robots_table, (gpointer)pos->GetId(), (void *)pos->Token() );

        pos->SetPropertyInt("charging", 0);


        pos->GetWorld()->GetModel("cave")->GetPropertyFloat("death_level",&death_level, death_level);
        pos->GetWorld()->GetModel("cave")->GetPropertyFloat("hunger_level",&hunger_level, hunger_level);

        pos->SetPropertyFloat("death_level", death_level);
        pos->SetPropertyFloat("hunger_level", hunger_level);
    }

    void Dock()
    {
        // close the grippers so they can be pushed into the charger
        ModelGripper::config_t gripper_data = gripper->GetConfig();

        if ( gripper_data.paddles != ModelGripper::PADDLE_CLOSED )
            gripper->CommandClose();
        else  if ( gripper_data.lift != ModelGripper::LIFT_UP )
            gripper->CommandUp();

        if ( charger_ahoy )
        {
            double a_goal = normalize( charger_bearing );

            // 		if( pos->Stalled() )
            //  		  {
            // 			 puts( "stalled. stopping" );
            //  			 pos->Stop();
            //		  }
            // 		else

            if ( charger_range > 0.5 )
            {
                if ( !ObstacleAvoid() )
                {
                    pos->SetXSpeed( cruisespeed );
                    pos->SetTurnSpeed( a_goal );
                }
            }
            else
            {
                pos->SetTurnSpeed( a_goal );
                pos->SetXSpeed( 0.02 );	// creep towards it

                if ( charger_range < 0.09 ) // close enough
                {
                    pos->Stop();
                }

                if ( pos->Stalled() ) // touching
                    pos->SetXSpeed( -0.01 ); // back off a bit
            }
        }
        else
        {
            //printf( "docking but can't see a charger\n" );
            pos->Stop();
            mode = MODE_WORK; // should get us back on track eventually
        }

        // if the battery is charged, go back to work
        if ( Full() )
        {
            //printf( "fully charged, now back to work\n" );
            mode = MODE_UNDOCK;
        }else{
            pos->SetPropertyInt("charging", 1);
        }
    }


    void UnDock()
    {
        const stg_meters_t gripper_distance = 0.2;
        const stg_meters_t back_off_distance = 0.4;
        const stg_meters_t back_off_speed = -0.05;

        // back up a bit
        if ( charger_range < back_off_distance )
            pos->SetXSpeed( back_off_speed );
        else
            pos->SetXSpeed( 0.0 );

        // once we have backed off a bit, open and lower the gripper
        ModelGripper::config_t gripper_data = gripper->GetConfig();
        if ( charger_range > gripper_distance )
        {
            if ( gripper_data.paddles != ModelGripper::PADDLE_OPEN )
                gripper->CommandOpen();
            else if ( gripper_data.lift != ModelGripper::LIFT_DOWN )
                gripper->CommandDown();
        }

        // if the gripper is down and open and we're away from the charger, undock is finished
        if ( gripper_data.paddles == ModelGripper::PADDLE_OPEN &&
                gripper_data.lift == ModelGripper::LIFT_DOWN &&
                charger_range > back_off_distance )
            mode = MODE_WORK;
    }

    bool ObstacleAvoid()
    {
        bool obstruction = false;
        bool stop = false;

        // find the closest distance to the left and right and check if
        // there's anything in front
        double minleft = 1e6;
        double minright = 1e6;

        // Get the data
        uint32_t sample_count=0;
        stg_laser_sample_t* scan = laser->GetSamples( &sample_count );

        for (uint32_t i = 0; i < sample_count; i++)
        {
            if ( verbose ) printf( "%.3f ", scan[i].range );

            if ( (i > (sample_count/4))
                    && (i < (sample_count - (sample_count/4)))
                    && scan[i].range < minfrontdistance)
            {
                if ( verbose ) puts( "  obstruction!" );
                obstruction = true;
            }

            if ( scan[i].range < stopdist )
            {
                if ( verbose ) puts( "  stopping!" );
                stop = true;
            }

            if ( i > sample_count/2 )
                minleft = MIN( minleft, scan[i].range );
            else
                minright = MIN( minright, scan[i].range );
        }

        if ( verbose )
        {
            puts( "" );
            printf( "minleft %.3f \n", minleft );
            printf( "minright %.3f\n ", minright );
        }

        if ( obstruction || stop || (avoidcount>0) )
        {
            if ( verbose ) printf( "Avoid %d\n", avoidcount );

            pos->SetXSpeed( stop ? 0.0 : avoidspeed );

            /* once we start avoiding, select a turn direction and stick
                with it for a few iterations */
            if ( avoidcount < 1 )
            {
                if ( verbose ) puts( "Avoid START" );
                avoidcount = random() % avoidduration + avoidduration;

                if ( minleft < minright  )
                {
                    pos->SetTurnSpeed( -avoidturn );
                    if ( verbose ) printf( "turning right %.2f\n", -avoidturn );
                }
                else
                {
                    pos->SetTurnSpeed( +avoidturn );
                    if ( verbose ) printf( "turning left %2f\n", +avoidturn );
                }
            }

            avoidcount--;

            return true; // busy avoding obstacles
        }

        return false; // didn't have to avoid anything
    }

    void Die()
    {
        // Default case
        pos->SetFiducialReturn(0);

        // override mode check
        int override;
        pos->GetPropertyInt("charging", &override, 0);

        if(override){
            mode = MODE_WORK;
        }else{
            pos->Stop();
            pos->SetFiducialReturn((int)pos->GetId());
        }
    }

    void Work()
    {
        if ( ! ObstacleAvoid() )
        {
            int override;
            pos->GetPropertyInt("charging", &override, 0);

            if ( Dead() && !override)
            {
                mode = MODE_DEAD;
                return;
            }

            if ( verbose ) puts( "Cruise" );

            ModelGripper::config_t gdata = gripper->GetConfig();

            //avoidcount = 0;
            pos->SetXSpeed( cruisespeed );

            Pose pose = pos->GetPose();

            int x = (pose.x + 8) / 4;
            int y = (pose.y + 8) / 4;

            // oh what an awful bug - 5 hours to track this down. When using
            // this controller in a world larger than 8*8 meters, a_goal can
            // sometimes be NAN. Causing trouble WAY upstream.
            if ( x > 3 ) x = 3;
            if ( y > 3 ) y = 3;
            if ( x < 0 ) x = 0;
            if ( y < 0 ) y = 0;

            double a_goal =
                dtor( ( pos->GetFlagCount() || gdata.gripped ) ? have[y][x] : need[y][x] );

            // if we are low on juice - find the direction to the recharger instead
            if ( Hungry() )
            {
                //puts( "hungry - using refuel map" );

                // use the refuel map
                a_goal = dtor( refuel[y][x] );

                if ( charger_ahoy ) // I see a charger while hungry!
                    mode = MODE_DOCK;
            } else {
                if ( ! at_dest )
                {
                    if ( gdata.beam[0] ) // inner break beam broken
                        gripper->CommandClose();
                }

                pos->SetPropertyInt("charging", 0);
            }

            assert( ! isnan(a_goal ) );
            assert( ! isnan(pose.a ) );

            double a_error = normalize( a_goal - pose.a );

            assert( ! isnan(a_error) );

            pos->SetTurnSpeed(  a_error );
        }
    }


    // inspect the laser data and decide what to do
    static int LaserUpdate( ModelLaser* laser, Robot* robot )
    {
        //   if( laser->power_pack && laser->power_pack->charging )
        // 	 printf( "model %s power pack @%p is charging\n",
        // 				laser->Token(), laser->power_pack );

        if ( laser->GetSamples(NULL) == NULL )
            return 0;

        switch ( robot->mode )
        {
        case MODE_DOCK:
            //puts( "DOCK" );
            robot->Dock();
            break;

        case MODE_WORK:
            //puts( "WORK" );
            robot->Work();
            break;

        case MODE_UNDOCK:
            //puts( "UNDOCK" );
            robot->UnDock();
            break;

        case MODE_DEAD:
            robot->Die();
            break;

        default:
            printf( "unrecognized mode %u\n", robot->mode );
        }

        return 0;
    }

    bool Dead()
    {
        int override;
        pos->GetPropertyInt("charging", &override, 0);

        if(override)
            return false;

        return( pos->FindPowerPack()->ProportionRemaining() < death_level );
    }

    bool Hungry()
    {
        return( pos->FindPowerPack()->ProportionRemaining() < hunger_level );
    }

    bool Full()
    {
        return( pos->FindPowerPack()->ProportionRemaining() > 0.95 );
    }

    static int PositionUpdate( ModelPosition* pos, Robot* robot )
    {
        Pose pose = pos->GetPose();

        //printf( "Pose: [%.2f %.2f %.2f %.2f]\n",
        //  pose.x, pose.y, pose.z, pose.a );

        //pose.z += 0.0001;
        //robot->pos->SetPose( pose );

        if ( pos->GetFlagCount() < payload &&
                hypot( -7-pose.x, -7-pose.y ) < 2.0 )
        {
            if ( ++robot->work_get > workduration )
            {
                // protect source from concurrent access
                robot->source->Lock();

                // transfer a chunk from source to robot
                pos->PushFlag( robot->source->PopFlag() );
                robot->source->Unlock();

                robot->work_get = 0;
            }
        }

        robot->at_dest = false;

        if ( hypot( 7-pose.x, 7-pose.y ) < 1.0 )
        {
            robot->at_dest = true;

            robot->gripper->CommandOpen();

            if ( ++robot->work_put > workduration )
            {
                // protect sink from concurrent access
                robot->sink->Lock();

                //puts( "dropping" );
                // transfer a chunk between robot and goal
                robot->sink->PushFlag( pos->PopFlag() );

                int c;
                robot->sink->GetPropertyInt("count", &c, 0);
                robot->sink->SetPropertyInt("count", c + 1);

                robot->sink->Unlock();

                robot->work_put = 0;
            }
        }


        return 0; // run again
    }



    static int FiducialUpdate( ModelFiducial* mod, Robot* robot )
    {
        robot->charger_ahoy = false;

        for ( unsigned int i = 0; i < mod->fiducial_count; i++ )
        {
            stg_fiducial_t* f = &mod->fiducials[i];

            //printf( "fiducial %d is %d at %.2f m %.2f radians\n",
            //	  i, f->id, f->range, f->bearing );

            if ( f->id == 2 ) // I see a charging station
            {
                // record that I've seen it and where it is
                robot->charger_ahoy = true;
                robot->charger_bearing = f->bearing;
                robot->charger_range = f->range;
                robot->charger_heading = f->geom.a;

                //printf( "charger at %.2f radians\n", robot->charger_bearing );
                break;
            }
        }

        return 0; // run again
    }

    static int BlobFinderUpdate( ModelBlobfinder* blobmod, Robot* robot )
    {
        unsigned int blob_count = 0;
        stg_blobfinder_blob_t* blobs = blobmod->GetBlobs( &blob_count );

        if ( blobs && (blob_count>0) )
        {
            printf( "%s sees %u blobs\n", blobmod->Token(), blob_count );
        }

        return 0;
    }

    static int GripperUpdate( ModelGripper* grip, Robot* robot )
    {
        ModelGripper::config_t gdata = grip->GetConfig();

        printf( "BREAKBEAMS %s %s\n",
                gdata.beam[0] ? gdata.beam[0]->Token() : "<null>",
                gdata.beam[1] ? gdata.beam[1]->Token() : "<null>" );

        printf( "CONTACTS %s %s\n",
                gdata.contact[0] ? gdata.contact[0]->Token() : "<null>",
                gdata.contact[1] ? gdata.contact[1]->Token() : "<null>");


        return 0;
    }


};


// Stage calls this when the model starts up
extern "C" int Init( Model* mod )
{
    Robot* robot = new Robot( (ModelPosition*)mod,
                              mod->GetWorld()->GetModel( "source" ),
                              mod->GetWorld()->GetModel( "sink" ) );
    return 0; //ok
}



