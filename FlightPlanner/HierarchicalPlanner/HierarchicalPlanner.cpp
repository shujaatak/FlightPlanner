#include "HierarchicalPlanner.h"

#include "guts/Conversions.h"

#include "QVectorND.h"
#include "SubFlightPlanner/SubFlightPlanner.h"
#include "SubFlightPlanner/SubFlightNode.h"
#include "RRTIntermediatePlanner/RRTIntermediatePlanner.h"
#include "PhonyIntermediatePlanner/PhonyIntermediatePlanner.h"

#include <QMap>
#include <cmath>

const qreal EVERY_X_METERS = 30.0;
const qreal AIRSPEED = 14.0; //meters per second
const qreal TIMESLICE = 15.0; //seconds
const qreal MAX_TURN_ANGLE = 3.14159265 / 4.0;

HierarchicalPlanner::HierarchicalPlanner(QSharedPointer<PlanningProblem> prob,
                                         QObject *parent) :
    FlightPlanner(prob, parent)
{
    this->doReset();
}

//protected
//pure-virtual from FlightPlanner
void HierarchicalPlanner::doStart()
{
}

//protected
//pure-virtual from FlightPlanner
void HierarchicalPlanner::doIteration()
{
    /*
     * Decide on arbitrary start and end points for each task (except no-fly).
     * They should be on edges of the polygon.
    */
    _buildStartAndEndPositions();

    /*
     * Calculate sub-flights from the global start point to each of the tasks' start points.
     * Also calculate sub-flights from each task's end point to every other tasks' start point.
    */
    _buildStartTransitions();

    /*
     * Calculate ideal sub-flights for each task (except no-fly).
     * These sub-flights start and end at the arbitrary start/end points of the tasks.
    */
    _buildSubFlights();


    /*
     * Build and solve scheduling problem.
    */
    _buildSchedule();
    this->pausePlanning();
}

//protected
//pure-virtual from FlightPlanner
void HierarchicalPlanner::doReset()
{
    _tasks.clear();
    _tasks2areas.clear();
    _areaStartPositions.clear();
    _areaStartOrientations.clear();
    _taskSubFlights.clear();
    _startTransitionSubFlights.clear();
    _obstacles.clear();

    if (this->problem().isNull())
        return;

    //Fill in list of tasks and mapping of tasks to areas
    foreach(const QSharedPointer<FlightTaskArea>& area, this->problem()->areas())
    {
        foreach(const QSharedPointer<FlightTask>& task, area->tasks())
        {
            //We treat obstacles separately in the hierarchical planner. Not as tasks.
            if (task->taskType() == "No-Fly Zone")
                _obstacles.append(area->geoPoly());
            else
            {
                _tasks.append(task);
                _tasks2areas.insert(task, area);
            }
        }
    }
}

//private
void HierarchicalPlanner::_buildStartAndEndPositions()
{
    //First calculate the average of all of the task area's midpoints
    //(Or at least an approximation based on their bounding rectangles...)
    QPointF avgLonLat(0.0,0.0);
    foreach(const QSharedPointer<FlightTaskArea>& area, _tasks2areas.values())
        avgLonLat += area->geoPoly().boundingRect().center();
    if (_tasks2areas.values().size() > 0)
        avgLonLat /= _tasks2areas.values().size();
    const QVector3D avgXYZ = Conversions::lla2xyz(Position(avgLonLat));

    //Then loop through all of the areas and find good points that could be start or end
    //Make the start point the one that is closest to the average computed above
    const qreal divisions = 100;
    foreach(const QSharedPointer<FlightTaskArea>& area, _tasks2areas.values())
    {
        const QRectF boundingRect = area->geoPoly().boundingRect();
        const QPointF centerLonLat = boundingRect.center();
        avgLonLat += centerLonLat;

        qreal mostDistance = std::numeric_limits<qreal>::min();
        QPointF bestPoint1;
        QPointF bestPoint2;
        for (int angleDeg = 0; angleDeg < 179; angleDeg++)
        {
            bool gotPos = false;
            bool gotNeg = false;

            const qreal stepSize = qMax<qreal>(boundingRect.width() / divisions,
                                               boundingRect.height() / divisions);
            const QVector2D dirVec(cos(angleDeg * 180.0 / 3.14159265),
                                   sin(angleDeg * 180.0 / 3.14159265));

            int count = 0;
            QPointF pos;
            QPointF neg;
            while (!gotPos || !gotNeg)
            {
                const QPointF trialPointPos(centerLonLat.x() + dirVec.x() * stepSize * count,
                                            centerLonLat.y() + dirVec.y() * stepSize * count);
                const QPointF trialPointNeg(centerLonLat.x() - dirVec.x() * stepSize * count,
                                            centerLonLat.y() - dirVec.y() * stepSize * count);

                if (!area->geoPoly().containsPoint(trialPointPos, Qt::OddEvenFill)
                        && !gotPos)
                {
                    pos = trialPointPos;
                    gotPos = true;
                }

                if (!area->geoPoly().containsPoint(trialPointNeg, Qt::OddEvenFill)
                        && !gotNeg)
                {
                    neg = trialPointNeg;
                    gotNeg = true;
                }
                count++;
            }

            const QVector3D xyz1 = Conversions::lla2xyz(Position(pos));
            const QVector3D xyz2 = Conversions::lla2xyz(Position(neg));
            const qreal distance = (xyz1 - xyz2).lengthSquared();
            if (distance > mostDistance)
            {
                mostDistance = distance;
                bestPoint1 = pos;
                bestPoint2 = neg;
            }
        }

        Position start;
        Position end;

        //The point closest to all the other areas will be the start
        start = bestPoint2;
        end = bestPoint1;
        if ((bestPoint1 - avgLonLat).manhattanLength() < (bestPoint2 - avgLonLat).manhattanLength())
        {
            start = bestPoint1;
            end = bestPoint2;
        }

        _areaStartPositions.insert(area, start);

        qreal angleRads = atan2(end.latitude() - start.latitude(),
                                end.longitude() - start.longitude());
        UAVOrientation orientation(angleRads);
        _areaStartOrientations.insert(area, orientation);
    }
}

//private
void HierarchicalPlanner::_buildStartTransitions()
{
    const Position& globalStartPos = this->problem()->startingPosition();
    const UAVOrientation& globalStartPose = this->problem()->startingOrientation();

    foreach(const QSharedPointer<FlightTask>& task, _tasks)
    {
        const QSharedPointer<FlightTaskArea>& area = _tasks2areas.value(task);
        if (_startTransitionSubFlights.contains(area))
            continue;
        const Position& taskStartPos = _areaStartPositions.value(area);
        const UAVOrientation& taskStartPose = _areaStartOrientations.value(area);

        QList<Position> subFlight = _generateTransitionFlight(globalStartPos,
                                                              globalStartPose,
                                                              taskStartPos,
                                                              taskStartPose);

        _startTransitionSubFlights.insert(area, subFlight);
    }
}

//private
void HierarchicalPlanner::_buildSubFlights()
{
    foreach(const QSharedPointer<FlightTask>& task, _tasks)
    {
        const QSharedPointer<FlightTaskArea>& area = _tasks2areas.value(task);
        const Position& start = _areaStartPositions.value(area);
        const UAVOrientation& startPose = _areaStartOrientations.value(area);

        SubFlightPlanner planner(task, area, start, startPose);
        planner.plan();

        _taskSubFlights.insert(task, planner.results());
    }
}

//private
void HierarchicalPlanner::_buildSchedule()
{
    //First we need to know how long each of our sub-flights takes
    QList<qreal> taskTimes;
    foreach(const QSharedPointer<FlightTask>& task, _tasks)
    {
        const QList<Position>& subFlight = _taskSubFlights.value(task);

        //Time required is estimated to be the length of the path in meters divided by airspeed
        qreal timeRequired = subFlight.length() * EVERY_X_METERS / AIRSPEED;
        taskTimes.append(timeRequired);
    }

    //start and end states
    const QVectorND startState(_tasks.size());
    const QVectorND endState(taskTimes);

    qDebug() << "Schedule from" << startState << "to" << endState;

    //This hash stores child:parent relationships
    QHash<QVectorND, QVectorND> parents;

    //This hash stores node:(index of last task) relationships
    QHash<QVectorND, int> lastTasks;

    //This hash stores node:(transition flight to reach node) relationships
    QHash<QVectorND, QList<Position> > transitionFlights;

    QMultiMap<qreal, QVectorND> worklist;
    QSet<QVectorND> closedSet;
    worklist.insert(0, startState);

    QList<QVectorND> schedule;

    while (!worklist.isEmpty())
    {
        QMutableMapIterator<qreal, QVectorND> iter(worklist);
        iter.next();
        const qreal costKey = iter.key();
        const QVectorND state = iter.value();
        closedSet.insert(state);
        iter.remove();

        qDebug() << "At:" << state << "with cost" << costKey;

        if (state == endState)
        {
            qDebug() << "Done scheduling - traceback.";
            QVectorND current = state;
            while (true)
            {
                qDebug() << current;
                schedule.prepend(current);
                if (!parents.contains(current))
                    break;
                current = parents.value(current);
            }
            break;
        }

        //Generate possible transitions
        for (int i = 0; i < state.dimension(); i++)
        {
            QVectorND newState = state;
            newState[i] = qMin<qreal>(taskTimes[i], newState[i] + TIMESLICE);
            if (closedSet.contains(newState))
                continue;

            //Add newState to closed list so it is never regenerated
            closedSet.insert(newState);

            //newState's parent is state
            parents.insert(newState, state);
            lastTasks.insert(newState, i);

            /*
             * The cost is the distance in the state space (draw us toward end node)
             * plus transition penalties ("context switching")
             */
            qreal cost = (endState - state).manhattanDistance();
            if (!lastTasks.contains(state))
                cost += _startTransitionSubFlights.value(_tasks2areas[_tasks[i]]).length() * EVERY_X_METERS / AIRSPEED;
            else if (lastTasks.value(state) == i)
                cost += 0.0;
            else
            {
                //The task we're coming from and the task we're going to
                const QSharedPointer<FlightTask>& prevTask = _tasks.value(lastTasks.value(state));
                const QSharedPointer<FlightTask>& nextTask = _tasks.value(lastTasks.value(newState));

                //Get current position and pose
                Position startPos;
                UAVOrientation startPose;
                _interpolatePath(_taskSubFlights.value(prevTask),
                                 _areaStartOrientations.value(_tasks2areas.value(prevTask)),
                                 state[_tasks.indexOf(prevTask)],
                        &startPos,
                        &startPose);

                //Get position/pose of context switch destination
                Position endPos;
                UAVOrientation endPose;
                _interpolatePath(_taskSubFlights.value(nextTask),
                                 _areaStartOrientations.value(_tasks2areas.value(nextTask)),
                                 state[i],
                                 &endPos,
                                 &endPose);

                //Plan intermediate flight
                QList<Position> intermed = _generateTransitionFlight(startPos, startPose,
                                                                     endPos, endPose);
                cost += intermed.length() * EVERY_X_METERS / AIRSPEED;
                transitionFlights.insert(newState, intermed);
            }

            worklist.insert(cost, newState);
        } // Done generating transitions
    } // Done building schedule

    QVectorND prevInterval = schedule[0];
    schedule.removeFirst();

    QList<Position> path;
    foreach(const QVectorND& interval, schedule)
    {
        const int taskIndex = lastTasks.value(interval);
        const QSharedPointer<FlightTask> task = _tasks.value(taskIndex);
        const QSharedPointer<FlightTaskArea> area = _tasks2areas.value(task);

        if (prevInterval == startState)
            path.append(_startTransitionSubFlights.value(area));
        else if (lastTasks.value(prevInterval) != taskIndex)
            path.append(transitionFlights.value(interval));

        //Add the portion of the sub-flight that we care about
        const QList<Position>& subFlight = _taskSubFlights.value(task);
        const qreal startTime = prevInterval.val(taskIndex);
        const qreal endTime = interval.val(taskIndex);
        path.append(_getPathPortion(subFlight, startTime, endTime));


        prevInterval = interval;
    }

    this->setBestFlightSoFar(path);
}

//private
bool HierarchicalPlanner::_interpolatePath(const QList<Position> &path,
                                           const UAVOrientation &startingOrientation,
                                           qreal goalTime,
                                           Position *outPosition,
                                           UAVOrientation *outOrientation) const
{
    if (outPosition == 0 || outOrientation == 0)
    {
        qDebug() << "Can't interpolate: bad output position/orientation pointer(s).";
        return false;
    }
    else if (goalTime < 0.0)
    {
        qDebug() << "Can't interpolate: bad time.";
        return false;
    }
    else if (path.isEmpty())
    {
        qDebug() << "Can't interpolate: empty path.";
        return false;
    }
    else if (path.size() == 1)
    {
        *outPosition = path[0];
        *outOrientation = startingOrientation;
        return true;
    }

    //qDebug() << "Interpolate path of size" << path.size() << "to time" << goalTime;

    qreal distanceSoFar = 0.0;
    qreal timeSoFar = 0.0;

    for (int i = 1; i < path.size(); i++)
    {
        const Position& pos = path[i];
        const Position& lastPos = path[i-1];
        //const qreal distance = (Conversions::lla2xyz(pos) - Conversions::lla2xyz(lastPos)).length();
        const qreal intervalDistance = EVERY_X_METERS;
        distanceSoFar += intervalDistance;
        timeSoFar = distanceSoFar / AIRSPEED;

        //qDebug() << "step" << i << "time" << timeSoFar << "position" << pos << "last position" << lastPos << "distance" << intervalDistance << "overall" << distanceSoFar;

        if (timeSoFar >= goalTime || i == path.size() - 1)
        {
            const qreal lonPerMeter = Conversions::degreesLonPerMeter(pos.latitude());
            const qreal latPerMeter = Conversions::degreesLatPerMeter(pos.latitude());
            const qreal lastTime = timeSoFar - intervalDistance / AIRSPEED;
            const qreal ratio = (goalTime - lastTime) / (timeSoFar - lastTime);
            QVector2D dirVecMeters((pos.longitude() - lastPos.longitude()) / lonPerMeter,
                                   (pos.latitude() - lastPos.latitude()) / latPerMeter);
            const qreal distToGo = EVERY_X_METERS * ratio;
            dirVecMeters.normalize();
            const qreal longitude = lastPos.longitude() + distToGo * dirVecMeters.x() * lonPerMeter;
            const qreal latitude = lastPos.latitude() + distToGo * dirVecMeters.y() * latPerMeter;
            *outPosition = Position(longitude, latitude);
            *outOrientation = UAVOrientation(atan2(dirVecMeters.y(),
                                                   dirVecMeters.x()));
            break;
        }
    }

    if (timeSoFar < goalTime)
    {
        qDebug() << "Can't interpolate into future. Goal time" << goalTime << "but only reached" << timeSoFar;
        //return false;
    }
    return true;
}

//private
QList<Position> HierarchicalPlanner::_generateTransitionFlight(const Position &startPos,
                                                               const UAVOrientation &startPose,
                                                               const Position &endPos,
                                                               const UAVOrientation &endPose)
{
    qDebug() << "Intermediate from" << startPos << startPose.radians() << "to" << endPos << endPose.radians();
    //Adjust the positions backwards a little bit along their angles?

    /*
    IntermediatePlanner * intermed = new RRTIntermediatePlanner(startPos,
                                                                startPose,
                                                                endPos,
                                                                endPose,
                                                                _obstacles);
                                                                */
    IntermediatePlanner * intermed = new PhonyIntermediatePlanner(startPos, startPose,
                                                                  endPos, endPose,
                                                                  _obstacles);
    intermed->plan();
    QList<Position> toRet = intermed->results();
    delete intermed;

    return toRet;
}

//private
QList<Position> HierarchicalPlanner::_getPathPortion(const QList<Position> &path,
                                                     qreal portionStartTime,
                                                     qreal portionEndTime) const
{
    QList<Position> toRet;

    const int startingIndex = portionStartTime * AIRSPEED / EVERY_X_METERS;
    const int endingIndex = portionEndTime * AIRSPEED / EVERY_X_METERS;
    //const int count = endingIndex - startingIndex;

    for (int i = startingIndex; i < endingIndex; i++)
        toRet.append(path[i]);


    return toRet;
}
