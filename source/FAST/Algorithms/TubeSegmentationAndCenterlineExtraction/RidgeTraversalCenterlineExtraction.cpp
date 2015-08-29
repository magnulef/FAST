#include "RidgeTraversalCenterlineExtraction.hpp"
#include "FAST/Data/Image.hpp"
#include "FAST/Data/LineSet.hpp"
#include "FAST/Data/Segmentation.hpp"
#include <queue>
#include <vector>
#include <list>
#include <stack>
#include <boost/unordered_set.hpp>
using boost::unordered_set;
#include <boost/unordered_map.hpp>
using boost::unordered_map;

namespace fast {

RidgeTraversalCenterlineExtraction::RidgeTraversalCenterlineExtraction() {
    createInputPort<Image>(0);
    createInputPort<Image>(1);

    // These are not required: Used when centerlines from two TDF results are to be merged
    createInputPort<Image>(2, false);
    createInputPort<Image>(3, false);

    createOutputPort<LineSet>(0, OUTPUT_DEPENDS_ON_INPUT, 0);
    createOutputPort<Segmentation>(1, OUTPUT_DEPENDS_ON_INPUT, 0);
}

typedef struct point {
    float value;
    int x,y,z;
} point;

class PointComparison {
    public:
    bool operator() (const point &lhs, const point &rhs) const {
        return (lhs.value < rhs.value);
    }
};

inline float sign(float a) {
    return a < 0 ? -1.0f: 1.0f;
}

typedef struct CenterlinePoint {
    Vector3i pos;
    bool large;
    Vector3i previousPos;
} CenterlinePoint;

#define LPOS(a,b,c) (a)+(b)*(size.x())+(c)*(size.x()*size.y())
#define POS(pos) pos.x()+pos.y()*size.x()+pos.z()*size.x()*size.y()

float squaredMagnitude(ImageAccess::pointer& vectorFieldAccess, Vector3i position) {
    Vector3f vector = vectorFieldAccess->getVector(position).head(3);
    float magnitude = vector.norm();
    return magnitude;
}

float getNormalizedValue(ImageAccess::pointer& vectorField, Vector3i pos, uint component) {
    float magnitude = squaredMagnitude(vectorField, pos);
    if(magnitude == 0) {
        return 0;
    } else {
        return vectorField->getScalar(pos, component);
    }
}

Vector3f gradient(ImageAccess::pointer& vectorField, Vector3i pos, int volumeComponent, int dimensions) {
    float f100, f_100, f010, f0_10, f001, f00_1;
    Vector3i npos = pos;
    npos.x() += 1;
    f100 = getNormalizedValue(vectorField, npos, volumeComponent);
    npos.x() -= 2;
    f_100 = getNormalizedValue(vectorField, npos, volumeComponent);
    if(dimensions > 1) {
        npos = pos;
        npos.y() += 1;
        f010 = getNormalizedValue(vectorField, npos, volumeComponent);
        npos.y() -= 2;
        f0_10 = getNormalizedValue(vectorField, npos, volumeComponent);
    }
    if(dimensions > 2) {
        npos = pos;
        npos.z() += 1;
        f001 = getNormalizedValue(vectorField, npos, volumeComponent);
        npos.z() -= 2;
        f00_1 = getNormalizedValue(vectorField, npos, volumeComponent);
    }

    Vector3f grad(0.5f*(f100-f_100), 0.5f*(f010-f0_10), 0.5f*(f001-f00_1));


    return grad;
}

void sortEigenvaluesAndVectors(Vector3f* eigenvaluesOut, Matrix3f* eigenvectorsOut) {
    Vector3f eigenvalues = *eigenvaluesOut;
    Matrix3f eigenvectors = *eigenvectorsOut;

    // Find largest eigenvalue
    int largestIndex = 0;
    int smallestIndex = 0;
    for(int i = 1; i < 3; i++) {
        if(fabs(eigenvalues[i]) > fabs(eigenvalues[largestIndex])) {
            largestIndex = i;
        }
        if(fabs(eigenvalues[i]) < fabs(eigenvalues[smallestIndex])) {
            smallestIndex = i;
        }
    }
    Vector3i indexes(0,1,2);
    indexes(smallestIndex) = -1;
    indexes(largestIndex) = -1;
    int middleIndex = indexes.maxCoeff();

    eigenvectorsOut->col(0) = eigenvectors.col(smallestIndex);
    eigenvectorsOut->col(1) = eigenvectors.col(middleIndex);
    eigenvectorsOut->col(2) = eigenvectors.col(largestIndex);
    (*eigenvaluesOut)(0) = eigenvalues(smallestIndex);
    (*eigenvaluesOut)(1) = eigenvalues(middleIndex);
    (*eigenvaluesOut)(2) = eigenvalues(largestIndex);
}


Vector3f getTubeDirection(ImageAccess::pointer& vectorField, Vector3i pos, Vector3ui size) {

    // Do gradient on Fx, Fy and Fz and normalization
    Vector3f Fx = gradient(vectorField, pos,0,1);
    Vector3f Fy = gradient(vectorField, pos,1,2);
    Vector3f Fz = gradient(vectorField, pos,2,3);

    Matrix3f hessian = Matrix3f::Zero();
    hessian(0, 0) = Fx.x();
    hessian(1, 0) = Fy.x();
    hessian(0, 1) = Fy.x();
    hessian(1, 1) = Fy.y();
    hessian.col(2) = Fz;
    hessian.row(2) = Fz;

    Eigen::SelfAdjointEigenSolver<Matrix3f> es(hessian);
    Matrix3f eigenvectors = es.eigenvectors();
    Vector3f eigenvalues = es.eigenvalues();
    sortEigenvaluesAndVectors(&eigenvalues, &eigenvectors);
    return eigenvectors.col(0);
}

void doEigen(ImageAccess::pointer& vectorField, Vector3i pos, Vector3ui size, Vector3f* lambda, Vector3f* e1, Vector3f* e2, Vector3f* e3) {

    // Do gradient on Fx, Fy and Fz and normalization
    Vector3f Fx = gradient(vectorField, pos,0,1);
    Vector3f Fy = gradient(vectorField, pos,1,2);
    Vector3f Fz = gradient(vectorField, pos,2,3);

    Matrix3f hessian = Matrix3f::Zero();
    hessian(0, 0) = Fx.x();
    hessian(1, 0) = Fy.x();
    hessian(0, 1) = Fy.x();
    hessian(1, 1) = Fy.y();
    hessian.col(2) = Fz;
    hessian.row(2) = Fz;

    Eigen::SelfAdjointEigenSolver<Matrix3f> es(hessian);
    Matrix3f eigenvectors = es.eigenvectors();
    Vector3f eigenvalues = es.eigenvalues();
    sortEigenvaluesAndVectors(&eigenvalues, &eigenvectors);
    *lambda = eigenvalues;
    *e1 = eigenvectors.col(0);
    *e2 = eigenvectors.col(1);
    *e3 = eigenvectors.col(2);
}

void copyToLineSet(std::stack<CenterlinePoint> points, std::vector<Vector3f>& vertices, std::vector<Vector2ui>& lines) {
    while(!points.empty()) {
        CenterlinePoint point = points.top();
        points.pop();
        if(point.previousPos.x() != -1) {
            const uint pos = vertices.size();
            vertices.push_back(point.pos.cast<float>());
            vertices.push_back(point.previousPos.cast<float>());
            lines.push_back(Vector2ui(pos, pos+1));
        }
    }
}

void extractCenterlines(
        Image::pointer TDF,
        Image::pointer vectorField,
        int* centerlines,
        unordered_map<int, int>& centerlineDistances,
        unordered_map<int, std::stack<CenterlinePoint> >& centerlineStacks,
        std::vector<Vector3f>& vertices,
        std::vector<Vector2ui>& lines
    ) {
    ImageAccess::pointer TDFaccess = TDF->getImageAccess(ACCESS_READ);
    ImageAccess::pointer vectorFieldAccess = vectorField->getImageAccess(ACCESS_READ);
    Vector3ui size = TDF->getSize();

    float Thigh = 0.5;
    int Dmin = 4;//getParam(parameters, "min-distance");
    float Mlow = 0.02;
    float Tlow = 0.1;
    int maxBelowTlow = 4;
    float minMeanTube = 0.5;
    const int totalSize = size.x()*size.y()*size.z();

        // Create queue
    std::priority_queue<point, std::vector<point>, PointComparison> queue;

    std::cout << "Getting valid start points for centerline extraction.." << std::endl;
    // Collect all valid start points
    #pragma omp parallel for
    for(int z = 2; z < size.z()-2; z++) {
        for(int y = 2; y < size.y()-2; y++) {
            for(int x = 2; x < size.x()-2; x++) {
                if(TDFaccess->getScalar(Vector3i(x,y,z)) < Thigh)
                    continue;

                Vector3i pos(x,y,z);
                bool valid = true;
                for(int a = -1; a < 2; a++) {
                    for(int b = -1; b < 2; b++) {
                        for(int c = -1; c < 2; c++) {
                            Vector3i nPos(x+a,y+b,z+c);
                            if(squaredMagnitude(vectorFieldAccess, nPos) < squaredMagnitude(vectorFieldAccess, pos)) {
                                valid = false;
                                break;
                            }
                        }
                    }
                }

                if(valid) {
                    point p;
                    p.value = TDFaccess->getScalar(Vector3i(x,y,z));
                    p.x = x;
                    p.y = y;
                    p.z = z;
                    #pragma omp critical
                    queue.push(p);
                }
            }
        }
    }

    std::cout << "Processing " << queue.size() << " valid start points" << std::endl;
    if(queue.size() == 0) {
        throw Exception("no valid start points found");
    }
    static int counter = 1;




    while(!queue.empty()) {
        // Traverse from new start point
        point p = queue.top();
        queue.pop();

        // Has it been handled before?
        if(centerlines[LPOS(p.x,p.y,p.z)] == 1)
            continue;

        unordered_set<int> newCenterlines;
        newCenterlines.insert(LPOS(p.x,p.y,p.z));
        int distance = 1;
        int connections = 0;
        int prevConnection = -1;
        int secondConnection = -1;
        float meanTube = TDFaccess->getScalar(Vector3i(p.x,p.y,p.z));

        // Create new stack for this centerline
        std::stack<CenterlinePoint> stack;
        CenterlinePoint startPoint;
        startPoint.previousPos = Vector3i(-1, -1, -1);
        startPoint.pos.x() = p.x;
        startPoint.pos.y() = p.y;
        startPoint.pos.z() = p.z;

        stack.push(startPoint);

        // For each direction
        for(int direction = -1; direction < 3; direction += 2) {
            Vector3i previous = startPoint.pos;
            int belowTlow = 0;
            Vector3i position(p.x,p.y,p.z);
            Vector3f t_i = getTubeDirection(vectorFieldAccess, position, size)*direction;
            Vector3f t_i_1 = t_i;


            // Traverse
            while(true) {
                Vector3i maxPoint(0,0,0);

                // Check for out of bounds
                if(position.x() < 3 || position.x() > size.x()-3 || position.y() < 3 || position.y() > size.y()-3 || position.z() < 3 || position.z() > size.z()-3)
                    break;

                // Try to find next point from all neighbors
                for(int a = -1; a < 2; a++) {
                    for(int b = -1; b < 2; b++) {
                        for(int c = -1; c < 2; c++) {
                            Vector3i n(position.x()+a,position.y()+b,position.z()+c);
                            if((a == 0 && b == 0 && c == 0) || TDFaccess->getScalar(n) == 0.0f)
                                continue;

                            Vector3f dir = (n - position).cast<float>();
                            dir.normalize();
                            if(dir.dot(t_i) <= 0.1) // Maintain direction
                                continue;

                            //if(T.radius[POS(n)] >= 1.5f) {
                            // Is magnitude smaller than previous
                            if(maxPoint == Vector3i(0,0,0)) {
                                maxPoint = n;
                            } else if(1 - squaredMagnitude(vectorFieldAccess, n) > 1 - squaredMagnitude(vectorFieldAccess, maxPoint)) {
                                maxPoint = n;
                            }
                                /*
                            } else {
                                if(T.TDF[LPOS(n.x,n.y,n.z)]*M(n.x,n.y,n.z) > T.TDF[POS(maxPoint)]*M(maxPoint.x,maxPoint.y,maxPoint.z))
                                maxPoint = n;
                            }
                            */

                        }
                    }
                }

                if(maxPoint.x() + maxPoint.y() + maxPoint.z() > 0) {
                    // New maxpoint found, check it!
                    if(centerlines[POS(maxPoint)] > 0) {
                        // Hit an existing centerline
                        if(prevConnection == -1) {
                            prevConnection = centerlines[POS(maxPoint)];
                        } else {
                            if(prevConnection == centerlines[POS(maxPoint)]) {
                                // A loop has occured, reject this centerline
                                connections = 5;
                            } else {
                                secondConnection = centerlines[POS(maxPoint)];
                            }
                        }
                        break;
                    } else if(1 - squaredMagnitude(vectorFieldAccess, maxPoint) < Mlow || (belowTlow > maxBelowTlow && TDFaccess->getScalar(maxPoint) < Tlow)) {
                        // New point is below thresholds
                        break;
                    } else if(newCenterlines.count(POS(maxPoint)) > 0) {
                        // Loop detected!
                        break;
                    } else {
                        // Point is OK, proceed to add it and continue
                        if(TDFaccess->getScalar(maxPoint) < Tlow) {
                            belowTlow++;
                        } else {
                            belowTlow = 0;
                        }

                        // Update direction
                        //float3 e1 = getTubeDirection(T, maxPoint,size.x,size.y,size.z);

                        //TODO: check if all eigenvalues are negative, if so find the egeinvector that best matches
                        Vector3f lambda, e1, e2, e3;
                        doEigen(vectorFieldAccess, maxPoint, size, &lambda, &e1, &e2, &e3);
                        if((lambda.x() < 0 && lambda.y() < 0 && lambda.z() < 0)) {
                            if(fabs(t_i.dot(e3)) > fabs(t_i.dot(e2))) {
                                if(fabs(t_i.dot(e3)) > fabs(t_i.dot(e1))) {
                                    e1 = e3;
                                }
                            } else if(fabs(t_i.dot(e2)) > fabs(t_i.dot(e1))) {
                                e1 = e2;
                            }
                        }


                        float maintain_dir = sign(e1.dot(t_i));
                        Vector3f vec_sum;
                        vec_sum.x() = maintain_dir*e1.x() + t_i.x() + t_i_1.x();
                        vec_sum.y() = maintain_dir*e1.y() + t_i.y() + t_i_1.y();
                        vec_sum.z() = maintain_dir*e1.z() + t_i.z() + t_i_1.z();
                        vec_sum.normalize();
                        t_i_1 = t_i;
                        t_i = vec_sum;

                        // update position
                        position = maxPoint;
                        distance ++;
                        newCenterlines.insert(POS(maxPoint));
                        meanTube += TDFaccess->getScalar(maxPoint);

                        // Create centerline point
                        CenterlinePoint p;
                        p.pos = position;
                        p.previousPos = previous;
                        previous = position;
                        /*
                        if(T.radius[POS(p.pos)] > 3.0f) {
                            p.large = true;
                        } else {
                            p.large = false;
                        }
                        */

                        // Add point to stack
                        stack.push(p);
                    }
                } else {
                    // No maxpoint found, stop!
                    break;
                }

            } // End traversal
        } // End for each direction

        // Check to see if new traversal can be added
        std::cout << "Finished. Distance " << distance << " meanTube: " << meanTube/distance << std::endl;
        if(distance > Dmin && meanTube/distance > minMeanTube && connections < 2) {
            //std::cout << "Finished. Distance " << distance << " meanTube: " << meanTube/distance << std::endl;
            //std::cout << "------------------- New centerlines added #" << counter << " -------------------------" << std::endl;


            unordered_set<int>::iterator usit;
            if(prevConnection == -1) {
                // No connections
                for(usit = newCenterlines.begin(); usit != newCenterlines.end(); usit++) {
                    centerlines[*usit] = counter;
                }
                centerlineDistances[counter] = distance;
                centerlineStacks[counter] = stack;
                counter ++;
            } else {
                // The first connection

                std::stack<CenterlinePoint> prevConnectionStack = centerlineStacks[prevConnection];
                while(!stack.empty()) {
                    prevConnectionStack.push(stack.top());
                    stack.pop();
                }

                for(usit = newCenterlines.begin(); usit != newCenterlines.end(); usit++) {
                    centerlines[*usit] = prevConnection;
                }
                centerlineDistances[prevConnection] += distance;
                if(secondConnection != -1) {
                    // Two connections, move secondConnection to prevConnection
                    std::stack<CenterlinePoint> secondConnectionStack = centerlineStacks[secondConnection];
                    centerlineStacks.erase(secondConnection);
                    while(!secondConnectionStack.empty()) {
                        prevConnectionStack.push(secondConnectionStack.top());
                        secondConnectionStack.pop();
                    }

                    #pragma omp parallel for
                    for(int i = 0; i < totalSize;i++) {
                        if(centerlines[i] == secondConnection)
                            centerlines[i] = prevConnection;
                    }
                    centerlineDistances[prevConnection] += centerlineDistances[secondConnection];
                    centerlineDistances.erase(secondConnection);
                }

                centerlineStacks[prevConnection] = prevConnectionStack;
            }
        } // end if new point can be added
    } // End while queue is not empty
    std::cout << "Finished traversal" << std::endl;
}

void RidgeTraversalCenterlineExtraction::execute() {

    LineSet::pointer centerlineOutput = getStaticOutputData<LineSet>(0);
    Segmentation::pointer centerlineVolumeOutput = getStaticOutputData<Segmentation>(1);

    Image::pointer TDF = getStaticInputData<Image>(0);
    Vector3ui size = TDF->getSize();
    const int totalSize = size.x()*size.y()*size.z();
    int TreeMin = 20;

    // Create some data structures
    int * centerlines = new int[totalSize]();

    // Create a map of centerline distances
    unordered_map<int, int> centerlineDistances;

    // Create a map of centerline stacks
    unordered_map<int, std::stack<CenterlinePoint> > centerlineStacks;

    std::vector<Vector3f> vertices;
    std::vector<Vector2ui> lines;

    {
        Image::pointer vectorField = getStaticInputData<Image>(1);
        extractCenterlines(TDF, vectorField, centerlines, centerlineDistances, centerlineStacks, vertices, lines);
    }

    // TODO check to see if more than two inputs were provided, if so run again..
    if(getNrOfInputData() > 2) {
        Image::pointer TDF = getStaticInputData<Image>(2);
        Image::pointer vectorField = getStaticInputData<Image>(3);
        extractCenterlines(TDF, vectorField, centerlines, centerlineDistances, centerlineStacks, vertices, lines);
    }



    if(centerlineDistances.size() == 0) {
        //throw SIPL::SIPLException("no centerlines were extracted");
        char * returnCenterlines = new char[totalSize]();
        std::cout << "No centerlines were extracted" << std::endl;
        return;
    }
    std::cout << centerlineDistances.size() << " centerline extracted" << std::endl;

    // Find largest connected tree and all trees above a certain size
    unordered_map<int, int>::iterator it;
    int max = centerlineDistances.begin()->first;
    std::list<int> trees;
    for(it = centerlineDistances.begin(); it != centerlineDistances.end(); it++) {
        if(it->second > centerlineDistances[max])
            max = it->first;
        if(it->second > TreeMin)
            trees.push_back(it->first);
    }
    std::list<int>::iterator it2;
    // TODO: if use the method with TreeMin have to add them to centerlineStack also
    std::stack<CenterlinePoint> centerlineStack = centerlineStacks[max];
    for(it2 = trees.begin(); it2 != trees.end(); it2++) {
        copyToLineSet(centerlineStacks[*it2], vertices, lines);
        while(!centerlineStacks[*it2].empty()) {
            centerlineStack.push(centerlineStacks[*it2].top());
            centerlineStacks[*it2].pop();
        }
    }

    uchar * returnCenterlines = new uchar[totalSize]();
    // Mark largest tree with 1, and rest with 0
    #pragma omp parallel for
    for(int i = 0; i < totalSize;i++) {
        if(centerlines[i] == max) {
        //if(centerlines[i] > 0) {
            returnCenterlines[i] = 1;
        } else {
            bool valid = false;
            for(it2 = trees.begin(); it2 != trees.end(); it2++) {
                if(centerlines[i] == *it2) {
                    returnCenterlines[i] = 1;
                    valid = true;
                    break;
                }
            }
            if(!valid)
                returnCenterlines[i] = 0;

        }
    }

    delete[] centerlines;

    centerlineOutput->create(vertices, lines);
    centerlineVolumeOutput->create(size.x(), size.y(), size.z(), TYPE_UINT8, 1, getMainDevice(), returnCenterlines);
    SceneGraph::setParentNode(centerlineVolumeOutput, TDF);
}

}