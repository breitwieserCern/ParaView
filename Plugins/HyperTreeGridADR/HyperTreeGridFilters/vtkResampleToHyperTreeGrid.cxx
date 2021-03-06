/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkResampleToHyperTreeGrid.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/

#include "vtkResampleToHyperTreeGrid.h"

#include "vtkAbstractAccumulator.h"
#include "vtkAbstractArrayMeasurement.h"
#include "vtkBitArray.h"
#include "vtkBox.h"
#include "vtkCell.h"
#include "vtkCell3D.h"
#include "vtkCellData.h"
#include "vtkConvexPointSet.h"
#include "vtkDataArray.h"
#include "vtkDataSet.h"
#include "vtkDemandDrivenPipeline.h"
#include "vtkDoubleArray.h"
#include "vtkHexahedron.h"
#include "vtkHyperTree.h"
#include "vtkHyperTreeGrid.h"
#include "vtkHyperTreeGridNonOrientedCursor.h"
#include "vtkHyperTreeGridNonOrientedVonNeumannSuperCursor.h"
#include "vtkInformation.h"
#include "vtkInformationVector.h"
#include "vtkLongArray.h"
#include "vtkMath.h"
#include "vtkMathUtilities.h"
#include "vtkObjectFactory.h"
#include "vtkPoints.h"
#include "vtkPolygon.h"
#include "vtkPolyhedron.h"
#include "vtkStreamingDemandDrivenPipeline.h"
#include "vtkTuple.h"
#include "vtkVoxel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <vector>

vtkStandardNewMacro(vtkResampleToHyperTreeGrid);

//----------------------------------------------------------------------------
vtkResampleToHyperTreeGrid::vtkResampleToHyperTreeGrid()
{
  this->SetNumberOfInputPorts(1);
  this->SetNumberOfOutputPorts(1);

  this->BranchFactor = 2;
  this->MaxDepth = 1;
  this->ArrayMeasurement = nullptr;
  this->ArrayMeasurementDisplay = nullptr;
  this->DisplayScalarField = nullptr;
  this->Min = -std::numeric_limits<double>::infinity();
  this->Max = std::numeric_limits<double>::infinity();
  this->MaxCache = this->Max;
  this->MinCache = this->Min;
  this->MinimumNumberOfPointsInSubtree = 1;
  this->InRange = true;
  this->NoEmptyCells = false;

  this->Extrapolate = true;
}

//----------------------------------------------------------------------------
vtkResampleToHyperTreeGrid::~vtkResampleToHyperTreeGrid() = default;

//----------------------------------------------------------------------------
void vtkResampleToHyperTreeGrid::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);

  os << indent << "InRange (boolean): " << this->InRange << std::endl;
  os << indent << "Min: " << this->Min << std::endl;
  os << indent << "MinCache: " << this->MinCache << std::endl;
  os << indent << "Max: " << this->Max << std::endl;
  os << indent << "MaxCache: " << this->MaxCache << std::endl;
  os << indent << "MinimumNumberOfPointsInSubtree: " << this->MinimumNumberOfPointsInSubtree
     << std::endl;
  os << indent << "MaxDepth: " << this->MaxDepth << std::endl;
  os << indent << "NoEmptyCells (boolean): " << this->NoEmptyCells << std::endl;
  os << indent << "BranchFactor: " << this->BranchFactor << std::endl;
  os << indent << "MaxResolutionPerTree: " << this->MaxResolutionPerTree << std::endl;

  for (std::size_t i = 0; i < this->ResolutionPerTree.size(); ++i)
  {
    os << indent << "ResolutionPerTree[" << i << "]: " << this->ResolutionPerTree[i] << std::endl;
  }

  if (this->ArrayMeasurement)
  {
    os << indent << *(this->ArrayMeasurement) << std::endl;
  }
  else
  {
    os << indent << "No ArrayMeasurement" << std::endl;
  }

  if (this->ArrayMeasurementDisplay)
  {
    os << indent << *(this->ArrayMeasurementDisplay) << std::endl;
  }
  else
  {
    os << indent << "No ArrayMeasurementDisplay" << std::endl;
  }
}

//----------------------------------------------------------------------------
int vtkResampleToHyperTreeGrid::FillInputPortInformation(int, vtkInformation* info)
{
  // This filter uses the vtkDataSet cell traversal methods so it
  // suppors any data set type as input.
  info->Set(vtkAlgorithm::INPUT_REQUIRED_DATA_TYPE(), "vtkDataSet");
  return 1;
}

int vtkResampleToHyperTreeGrid::FillOutputPortInformation(int, vtkInformation* info)
{
  info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkHyperTreeGrid");
  return 1;
}

int vtkResampleToHyperTreeGrid::RequestInformation(
  vtkInformation*, vtkInformationVector**, vtkInformationVector* outputVector)
{
  // Get the information objects
  vtkInformation* outInfo = outputVector->GetInformationObject(0);

  // We cannot give the exact number of levels of the hypertrees
  // because it is not generated yet and this process depends on the recursion formula.
  // Just send an upper limit instead.
  outInfo->Set(vtkHyperTreeGrid::LEVELS(), this->MaxDepth);
  outInfo->Set(vtkHyperTreeGrid::DIMENSION(), 3);
  outInfo->Set(vtkHyperTreeGrid::ORIENTATION(), 0);

  return 1;
}

//----------------------------------------------------------------------------
int vtkResampleToHyperTreeGrid::RequestData(
  vtkInformation*, vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  this->UpdateProgress(0.0);

  // Get input and output data.
  vtkDataSet* input = vtkDataSet::GetData(inputVector[0]);
  vtkDataObject* outputDO = vtkDataObject::GetData(outputVector, 0);
  vtkHyperTreeGrid* output = vtkHyperTreeGrid::SafeDownCast(outputDO);
  if (!output)
  {
    vtkErrorMacro("Incorrect type of output: " << outputDO->GetClassName());
    return 0;
  }

  // Skip execution if there is no input geometry.
  vtkIdType numCells = input->GetNumberOfCells();
  vtkIdType numPts = input->GetNumberOfPoints();
  if (numCells < 1 || numPts < 1)
  {
    vtkDebugMacro("No data to convert!");
    return 1;
  }

  output->Initialize();
  output->SetBranchFactor(this->BranchFactor);

  double* bounds = input->GetBounds();

  // Setting the point locations for the hyper tree grid
  {
    vtkNew<vtkDoubleArray> coords;
    coords->SetNumberOfValues(this->Dimensions[0]);
    double step =
      this->Dimensions[0] > 1 ? (bounds[1] - bounds[0]) / (this->Dimensions[0] - 1) : 0.0;
    for (size_t i = 0; i < this->Dimensions[0]; ++i)
    {
      coords->SetValue(i, bounds[0] + step * i);
    }
    output->SetXCoordinates(coords);
  }

  {
    vtkNew<vtkDoubleArray> coords;
    coords->SetNumberOfValues(this->Dimensions[1]);
    double step =
      this->Dimensions[1] > 1 ? (bounds[3] - bounds[2]) / (this->Dimensions[1] - 1) : 0.0;
    for (size_t i = 0; i < this->Dimensions[1]; ++i)
    {
      coords->SetValue(i, bounds[2] + step * i);
    }
    output->SetYCoordinates(coords);
  }

  {
    vtkNew<vtkDoubleArray> coords;
    coords->SetNumberOfValues(this->Dimensions[2]);
    double step =
      this->Dimensions[2] > 1 ? (bounds[5] - bounds[4]) / (this->Dimensions[2] - 1) : 0.0;
    for (size_t i = 0; i < this->Dimensions[2]; ++i)
    {
      coords->SetValue(i, bounds[4] + step * i);
    }
    output->SetZCoordinates(coords);
  }

  output->SetDimensions(this->Dimensions);
  output->GetCellDims(this->CellDims);

  // Setting up a few useful values during the pipeline
  this->ResolutionPerTree.resize(this->MaxDepth + 1);
  this->Diagonal.resize(this->MaxDepth + 1);

  assert((this->MaxDepth + 1) && "Maximum depth has to be greater than one");

  this->ResolutionPerTree[0] = 1;
  this->Diagonal[0] =
    (bounds[1] - bounds[0]) * (bounds[1] - bounds[0]) / (this->CellDims[0] * this->CellDims[0]) +
    (bounds[3] - bounds[2]) * (bounds[3] - bounds[2]) / (this->CellDims[1] * this->CellDims[1]) +
    (bounds[5] - bounds[4]) * (bounds[5] - bounds[4]) / (this->CellDims[2] * this->CellDims[2]);

  for (std::size_t depth = 1; depth < this->ResolutionPerTree.size(); ++depth)
  {
    this->ResolutionPerTree[depth] = this->ResolutionPerTree[depth - 1] * this->BranchFactor;
    this->Diagonal[depth] = this->Diagonal[depth - 1] / (this->BranchFactor * this->BranchFactor);
  }
  this->MaxResolutionPerTree = this->ResolutionPerTree[this->MaxDepth];

  this->NumberOfChildren = std::pow(this->BranchFactor, output->GetDimension());

  this->Mask = vtkBitArray::New();

  // Linking input scalar fields
  vtkSmartPointer<vtkDataArray> data = this->GetInputArrayToProcess(0, inputVector);
  int fieldAssociation = this->GetInputArrayAssociation(0, inputVector);

  if (this->ArrayMeasurement)
  {
    vtkNew<vtkDoubleArray> scalarField;
    scalarField->SetName((std::string(data->GetName()) + std::string("_measure")).c_str());
    output->GetCellData()->AddArray(scalarField);
    this->ScalarField = scalarField;
  }

  if (this->ArrayMeasurementDisplay)
  {
    vtkNew<vtkDoubleArray> scalarFieldDisplay;
    scalarFieldDisplay->SetName(data->GetName());
    output->GetCellData()->AddArray(scalarFieldDisplay);
    this->DisplayScalarField = scalarFieldDisplay;
  }

  vtkNew<vtkLongArray> numberOfLeavesInSubtreeField;
  numberOfLeavesInSubtreeField->SetName("Number of leaves");
  output->GetCellData()->AddArray(numberOfLeavesInSubtreeField);
  this->NumberOfLeavesInSubtreeField = numberOfLeavesInSubtreeField;

  vtkNew<vtkLongArray> numberOfPointsInSubtreeField;
  numberOfPointsInSubtreeField->SetName("Number of points");
  output->GetCellData()->AddArray(numberOfPointsInSubtreeField);
  this->NumberOfPointsInSubtreeField = numberOfPointsInSubtreeField;

  if (this->ArrayMeasurement)
  {
    this->Accumulators = this->ArrayMeasurement->NewAccumulatorInstances();
    for (std::size_t i = 0; i < this->Accumulators.size(); ++i)
    {
      this->Accumulators[i]->DeepCopy(this->ArrayMeasurement->GetAccumulators()[i]);
    }
  }

  // If we have two array measurements to compute,
  // we create a vector of needed accumulator for both
  // measurement methods. This avoids computing the same
  // quantity several times
  if (this->ArrayMeasurementDisplay)
  {
    this->ArrayMeasurementAccumulators.resize(this->Accumulators.size(), nullptr);
    this->ArrayMeasurementDisplayAccumulators.resize(
      this->ArrayMeasurementDisplay->GetAccumulators().size());
    std::size_t size = this->Accumulators.size();
    for (auto& accumulator : this->ArrayMeasurementDisplay->GetAccumulators())
    {
      std::size_t i = 0;
      for (; i < size; ++i)
      {
        if (accumulator->HasSameParameters(this->Accumulators[i]))
        {
          break;
        }
      }
      if (i == size)
      {
        this->ArrayMeasurementDisplayAccumulatorMap.push_back(this->Accumulators.size());
        this->Accumulators.emplace_back(accumulator->NewInstance());
        this->Accumulators[this->Accumulators.size() - 1]->DeepCopy(accumulator);
      }
      else
      {
        this->ArrayMeasurementDisplayAccumulatorMap.push_back(i);
      }
    }
  }

  // Creating multi resolution grids used to construct the hyper tree grid
  // This multi resolution grid has the inner structure of the hyper tree grid
  // without its indexing. This is a bottom-up algorithm, which would be impossible
  // to process directly using a hyper tree grid because of its top-down structure.
  this->CreateGridOfMultiResolutionGrids(input, data, fieldAssociation);

  if (!this->GenerateTrees(output))
  {
    return 0;
  }

  output->SetMask(this->Mask);
  this->Mask->FastDelete();

  if (this->Extrapolate && fieldAssociation == vtkDataObject::FIELD_ASSOCIATION_POINTS)
  {
    vtkHyperTreeGrid* htg = vtkHyperTreeGrid::SafeDownCast(output);
    this->ExtrapolateValuesOnGaps(htg);
  }

  // Cleaning our mess
  this->GridOfMultiResolutionGrids.clear();
  for (std::size_t i = 0; i < this->Accumulators.size(); ++i)
  {
    this->Accumulators[i]->FastDelete();
    this->Accumulators[i] = nullptr;
  }
  this->Accumulators.clear();
  this->ArrayMeasurementDisplayAccumulators.clear();
  this->ArrayMeasurementAccumulators.clear();
  this->ArrayMeasurementDisplayAccumulatorMap.clear();

  // Avoid keeping extra memory around.
  output->Squeeze();

  this->UpdateProgress(1.0);

  return 1;
}

//----------------------------------------------------------------------------
vtkResampleToHyperTreeGrid::GridElement::~GridElement()
{
  for (std::size_t i = 0; i < this->Accumulators.size(); ++i)
  {
    this->Accumulators[i]->FastDelete();
  }
}

//----------------------------------------------------------------------------
bool vtkResampleToHyperTreeGrid::IntersectedVolume(
  const double boxBounds[6], vtkVoxel* voxel, double volumeUnit, double& volume) const
{
  const double* voxelBounds = voxel->GetBounds();
  double x = std::min(boxBounds[1], voxelBounds[1]) - std::max(boxBounds[0], voxelBounds[0]),
         y = std::min(boxBounds[3], voxelBounds[3]) - std::max(boxBounds[2], voxelBounds[2]),
         z = std::min(boxBounds[5], voxelBounds[5]) - std::max(boxBounds[4], voxelBounds[4]);
  const double min = std::pow(VTK_DBL_MIN, 1.0 / 3.0);
  double normalization = volumeUnit < 1.0 ? volumeUnit : 1.0;
  bool nonZeroVolume =
    x >= min / normalization && y >= min / normalization && z >= min / normalization;
  volume = nonZeroVolume ? (x * y * z) / volumeUnit : 0.0;
  return nonZeroVolume;
}

//----------------------------------------------------------------------------
bool vtkResampleToHyperTreeGrid::IntersectedVolume(const double bboxBounds[6], vtkCell3D* cell3D,
  double vtkNotUsed(volumeUnit), double& volume, double* weights) const
{
  static constexpr double identity[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
  double boxBounds[6] = { bboxBounds[0], bboxBounds[1], bboxBounds[2], bboxBounds[3], bboxBounds[4],
    bboxBounds[5] };
  std::array<std::set<double>, 12> duplicates;
  double boxVolume = 0.0;
  double centroid[3] = { 0.0 };
  double signedDistanceToCentroid = 0.0;
  std::vector<double> facePoints(cell3D->GetNumberOfPoints() * 3);
  double cellBounds[6];
  cell3D->GetBounds(cellBounds);
  vtkPoints* points = cell3D->Points;
  volume = 0.0;
  double p[3];
  for (int i = 0; i < 4; ++i)
  {
    points->GetPoint(i, p);
  }

  double p12[6], normal[3], x1[3], x2[3], edgeNormal[3], edgeBoxBound1[3], edgeBoxBound2[3],
    edgeNormalBoxBound1[3], edgeNormalBoxBound2[3], edgeNormalOnBox1[3], edgeNormalOnBox2[3];
  int subId;
  double dist2;
  double TOL = 1e-2;

  bool change;
  do
  {
    change = false;
    for (vtkIdType pointId = 0; pointId < cell3D->GetNumberOfPoints(); ++pointId)
    {
      points->GetPoint(pointId, p);
      if (std::abs(p[0] - boxBounds[0]) < TOL && p[1] <= boxBounds[3] + TOL &&
        p[1] >= boxBounds[2] - TOL && p[2] <= boxBounds[5] + TOL && p[2] >= boxBounds[4] - TOL)
      {
        boxBounds[0] -= TOL;
        change = true;
      }
      if (std::abs(p[0] - boxBounds[1]) < TOL && p[1] <= boxBounds[3] + TOL &&
        p[1] >= boxBounds[2] - TOL && p[2] <= boxBounds[5] + TOL && p[2] >= boxBounds[4] - TOL)
      {
        boxBounds[1] += TOL;
        change = true;
      }
      if (std::abs(p[1] - boxBounds[2]) < TOL && p[0] <= boxBounds[1] + TOL &&
        p[0] >= boxBounds[0] - TOL && p[2] <= boxBounds[5] + TOL && p[2] >= boxBounds[4] - TOL)
      {
        boxBounds[2] -= TOL;
        change = true;
      }
      if (std::abs(p[1] - boxBounds[3]) < TOL && p[0] <= boxBounds[1] + TOL &&
        p[0] >= boxBounds[0] - TOL && p[2] <= boxBounds[5] + TOL && p[2] >= boxBounds[4] - TOL)
      {
        boxBounds[3] += TOL;
        change = true;
      }
      if (std::abs(p[2] - boxBounds[4]) < TOL && p[0] <= boxBounds[1] + TOL &&
        p[0] >= boxBounds[0] - TOL && p[1] <= boxBounds[3] + TOL && p[1] >= boxBounds[2] - TOL)
      {
        boxBounds[4] -= TOL;
        change = true;
      }
      if (std::abs(p[2] - boxBounds[5]) < TOL && p[0] <= boxBounds[1] + TOL &&
        p[0] >= boxBounds[0] - TOL && p[1] <= boxBounds[3] + TOL && p[1] >= boxBounds[2] - TOL)
      {
        boxBounds[5] += TOL;
        change = true;
      }
    }
  } while (change);

  for (std::size_t boxVertexId = 0; boxVertexId < 8; ++boxVertexId)
  {
    x1[0] = boxBounds[boxVertexId & 1];
    x1[1] = boxBounds[2 + ((boxVertexId & 2) >> 1)];
    x1[2] = boxBounds[4 + ((boxVertexId & 4) >> 2)];

    if (cell3D->EvaluatePosition(x1, x2, subId, p12, dist2, weights))
    {
      bool sligthlyOutside = false;
      for (vtkIdType vertexId = 0; vertexId < cell3D->GetNumberOfPoints(); ++vertexId)
      {
        points->GetPoint(vertexId, p);
        if (weights[vertexId] < VTK_DBL_MIN)
        {
          sligthlyOutside = true;
        }
        else if (weights[vertexId] >= 1.0 - TOL)
          if (0)
          {
            switch (boxVertexId)
            {
              case 0:
                boxBounds[0] -= TOL;
                boxBounds[2] -= TOL;
                boxBounds[4] -= TOL;
                break;
              case 1:
                boxBounds[1] += TOL;
                boxBounds[2] -= TOL;
                boxBounds[4] -= TOL;
                break;
              case 2:
                boxBounds[1] += TOL;
                boxBounds[3] += TOL;
                boxBounds[4] -= TOL;
                break;
              case 3:
                boxBounds[0] -= TOL;
                boxBounds[3] += TOL;
                boxBounds[4] -= TOL;
                break;
              case 4:
                boxBounds[0] -= TOL;
                boxBounds[2] -= TOL;
                boxBounds[5] += TOL;
                break;
              case 5:
                boxBounds[1] += TOL;
                boxBounds[2] -= TOL;
                boxBounds[5] += TOL;
                break;
              case 6:
                boxBounds[1] += TOL;
                boxBounds[3] += TOL;
                boxBounds[5] += TOL;
                break;
              case 7:
                boxBounds[0] -= TOL;
                boxBounds[3] += TOL;
                boxBounds[5] += TOL;
                break;
            }
          }
      }
      if (!sligthlyOutside)
      {
        // Coefficient depending of the vertex of the box
        // -6_____6
        //  /|   /|
        // 6/_|-6/ |
        // |6|__|_|-6
        // |/   |/
        // /____/     x_ y/ z|
        //-6   6
        // !A != !B equivalent to A xor B
        boxVolume += (!(boxVertexId & 1) != !(boxVertexId & 2) ? 6.0 : -6.0) *
          ((boxVertexId & 4) ? -1.0 : 1.0) * x1[0] * x1[1] * x1[2];
      }
    }
  }

  for (vtkIdType vertexId = 0; vertexId < cell3D->GetNumberOfPoints(); ++vertexId)
  {
    points->GetPoint(vertexId, x1);
    centroid[0] += x1[0];
    centroid[1] += x1[1];
    centroid[2] += x1[2];
  }
  centroid[0] /= cell3D->GetNumberOfPoints();
  centroid[1] /= cell3D->GetNumberOfPoints();
  centroid[2] /= cell3D->GetNumberOfPoints();
  TOL = 1e-6;

  for (vtkIdType faceId = 0; faceId < cell3D->GetNumberOfFaces(); ++faceId)
  {
    const vtkIdType* pts;
    double *p1, *p2;
    double t1, t2;
    int plane1, plane2;
    cell3D->GetFacePoints(faceId, pts);
    vtkIdType faceSize = ~0;
    while (pts[++faceSize] != -1)
      ;
    if (faceSize > 2)
    {
      vtkPolygon::ComputeNormal(cell3D->Points, faceSize, pts, normal);
      points->GetPoint(pts[faceSize - 1], facePoints.data() + (faceSize - 1) * 3);
      points->GetPoint(pts[0], facePoints.data());
      points->GetPoint(pts[1], facePoints.data() + 3);
      signedDistanceToCentroid +=
        vtkMath::Dot(normal, centroid) - vtkMath::Dot(normal, facePoints.data());
      for (vtkIdType idx1 = 0, idx2 = 1; idx1 < faceSize; ++idx1, idx2 = (idx2 + 1) % faceSize)
      {
        if (idx1 < faceSize - 3)
        {
          points->GetPoint(pts[idx1 + 2], facePoints.data() + (idx1 + 1) * 3);
        }
        p1 = facePoints.data() + idx1 * 3;
        p2 = facePoints.data() + idx2 * 3;

        if (!(vtkMathUtilities::NearlyEqual(p2[0], p1[0]) &&
              vtkMathUtilities::NearlyEqual(p2[1], p1[1]) &&
              vtkMathUtilities::NearlyEqual(p2[2], p1[2])))
        {
          double tangent[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
          vtkMath::Normalize(tangent);
          vtkMath::Cross(normal, tangent, edgeNormal);
          bool p1InsideNode = p1[0] > boxBounds[0] &&
            !vtkMathUtilities::NearlyEqual(p1[0], boxBounds[0]) && p1[0] < boxBounds[1] &&
            !vtkMathUtilities::NearlyEqual(p1[0], boxBounds[1]) && p1[1] > boxBounds[2] &&
            !vtkMathUtilities::NearlyEqual(p1[1], boxBounds[2]) && p1[1] < boxBounds[3] &&
            !vtkMathUtilities::NearlyEqual(p1[1], boxBounds[3]) && p1[2] > boxBounds[4] &&
            !vtkMathUtilities::NearlyEqual(p1[2], boxBounds[4]) && p1[2] < boxBounds[5] &&
            !vtkMathUtilities::NearlyEqual(p1[2], boxBounds[5]),
               p2InsideNode = p2[0] >= boxBounds[0] &&
            !vtkMathUtilities::NearlyEqual(p2[0], boxBounds[0]) && p2[0] < boxBounds[1] &&
            !vtkMathUtilities::NearlyEqual(p2[0], boxBounds[1]) && p2[1] > boxBounds[2] &&
            !vtkMathUtilities::NearlyEqual(p2[1], boxBounds[2]) && p2[1] < boxBounds[3] &&
            !vtkMathUtilities::NearlyEqual(p2[1], boxBounds[3]) && p2[2] > boxBounds[4] &&
            !vtkMathUtilities::NearlyEqual(p2[2], boxBounds[4]) && p2[2] < boxBounds[5] &&
            !vtkMathUtilities::NearlyEqual(p2[2], boxBounds[5]);
          if (p1InsideNode)
          {
            boxVolume +=
              vtkMath::Dot(p1, tangent) * vtkMath::Dot(p1, edgeNormal) * vtkMath::Dot(p1, normal);
          }
          if (p2InsideNode)
          {
            boxVolume -=
              vtkMath::Dot(p2, tangent) * vtkMath::Dot(p2, edgeNormal) * vtkMath::Dot(p2, normal);
          }
          if ((!p1InsideNode || !p2InsideNode) &&
            vtkBox::IntersectWithInfiniteLine(boxBounds, p1, p2, t1, t2, x1, x2, plane1, plane2) &&
            !vtkMathUtilities::NearlyEqual(t1, t2))
          {
            if (t1 >= 0.0 && t1 + VTK_DBL_EPSILON <= 1.0)
            {
              vtkMath::Cross(identity + 3 * (plane1 / 2), normal, edgeBoxBound1);
              vtkMath::Normalize(edgeBoxBound1);
              vtkMath::Cross(normal, edgeBoxBound1, edgeNormalBoxBound1);
              boxVolume +=
                vtkMath::Dot(x1, tangent) * vtkMath::Dot(x1, edgeNormal) * vtkMath::Dot(x1, normal);
              boxVolume -= vtkMath::Dot(x1, edgeBoxBound1) * vtkMath::Dot(x1, edgeNormalBoxBound1) *
                vtkMath::Dot(x1, normal);
              vtkMath::Cross(identity + 3 * (plane1 / 2), edgeBoxBound1, edgeNormalOnBox1);
              volume += vtkMath::Dot(x1, edgeBoxBound1) * x1[plane1 / 2] *
                vtkMath::Dot(x1, edgeNormalOnBox1);
            }
            if (t2 >= VTK_DBL_MIN && t2 <= 1.0)
            {
              vtkMath::Cross(identity + 3 * (plane2 / 2), normal, edgeBoxBound2);
              vtkMath::Normalize(edgeBoxBound2);
              vtkMath::Cross(normal, edgeBoxBound2, edgeNormalBoxBound2);
              boxVolume -=
                vtkMath::Dot(x2, tangent) * vtkMath::Dot(x2, edgeNormal) * vtkMath::Dot(x2, normal);
              boxVolume += vtkMath::Dot(x2, edgeBoxBound2) * vtkMath::Dot(x2, edgeNormalBoxBound2) *
                vtkMath::Dot(x2, normal);
              vtkMath::Cross(identity + 3 * (plane2 / 2), edgeBoxBound2, edgeNormalOnBox2);
              volume -= vtkMath::Dot(x2, edgeBoxBound2) * x2[plane2 / 2] *
                vtkMath::Dot(x2, edgeNormalOnBox2);
            }
          }
        }
      }
      double d = -vtkMath::Dot(normal, facePoints.data());
      for (std::size_t dim = 0; dim < 3; ++dim)
      {
        vtkMath::Cross(normal, identity + (3 * (dim + 1)) % 9, edgeBoxBound1);
        vtkMath::Normalize(edgeBoxBound1);
        vtkMath::Cross(normal, identity + (3 * (dim + 2)) % 9, edgeBoxBound2);
        vtkMath::Normalize(edgeBoxBound2);
        vtkMath::Cross(edgeBoxBound1, normal, edgeNormalBoxBound1);
        vtkMath::Cross(edgeBoxBound2, normal, edgeNormalBoxBound2);

        // On edges orthogonal to dim (seen as a vertex when slicing a box with plane at constant
        // dim.
        //  ____
        // |    |
        // |    |
        //>|____|
        // ^
        p12[(dim + 1) % 3] = boxBounds[(2 * (dim + 1)) % 6];
        p12[(dim + 2) % 3] = boxBounds[(2 * (dim + 2)) % 6];
        p12[dim] = std::abs(normal[dim]) >= VTK_DBL_EPSILON
          ? -1.0 / normal[dim] * (d + p12[(dim + 1) % 3] * normal[(dim + 1) % 3] +
                                   p12[(dim + 2) % 3] * normal[(dim + 2) % 3])
          : std::numeric_limits<double>::infinity();
        auto it = std::min_element(duplicates[dim * 4].begin(), duplicates[dim * 4].end(),
          [&](double x, double y) { return std::fabs(x - p12[dim]) < std::fabs(y - p12[dim]); });
        if ((it == duplicates[dim * 4].end() ||
              (it != duplicates[dim * 4].end() && std::fabs(*it - p12[dim]) > TOL)) &&
          ((p12[dim] >= boxBounds[2 * dim] && p12[dim] <= boxBounds[2 * dim + 1]) ||
              (vtkMathUtilities::NearlyEqual(p12[dim], boxBounds[2 * dim]) &&
                vtkMathUtilities::NearlyEqual(p12[dim], boxBounds[2 * dim + 1]))) &&
          vtkPolygon::PointInPolygon(p12, faceSize, facePoints.data(), cellBounds, normal))
        {
          volume += (normal[dim] > 0.0 ? 1.0 : -1.0) * vtkMath::Dot(p12, edgeBoxBound1) *
            vtkMath::Dot(p12, edgeNormalBoxBound1) * vtkMath::Dot(p12, normal);
          vtkMath::Cross(edgeBoxBound1, identity + (3 * (dim + 1)) % 9, edgeNormalOnBox1);
          volume -= (edgeBoxBound1[(dim + 2) % 3] > 0.0 ? 1.0 : -1.0) *
            vtkMath::Dot(p12, edgeBoxBound1) * p12[(dim + 1) % 3] *
            vtkMath::Dot(p12, edgeNormalOnBox1);
          volume += (normal[dim] < 0.0 ? 1.0 : -1.0) * vtkMath::Dot(p12, edgeBoxBound2) *
            vtkMath::Dot(p12, edgeNormalBoxBound2) * vtkMath::Dot(p12, normal);
          vtkMath::Cross(edgeBoxBound2, identity + (3 * (dim + 2)) % 9, edgeNormalOnBox2);
          volume -= (edgeBoxBound2[(dim + 1) % 3] > 0.0 ? 1.0 : -1.0) *
            vtkMath::Dot(p12, edgeBoxBound2) * p12[(dim + 2) % 3] *
            vtkMath::Dot(p12, edgeNormalOnBox2);
          volume += (normal[dim] > 0.0 ? 2.0 : -2.0) * p12[0] * p12[1] * p12[2];
        }
        duplicates[dim * 4].emplace(p12[dim]);

        //  ____
        // |    |
        // |    |
        // |____|<
        //      ^
        p12[(dim + 1) % 3] = boxBounds[(2 * (dim + 1) + 1) % 6];
        p12[dim] = std::abs(normal[dim]) >= VTK_DBL_EPSILON
          ? -1.0 / normal[dim] * (d + p12[(dim + 1) % 3] * normal[(dim + 1) % 3] +
                                   p12[(dim + 2) % 3] * normal[(dim + 2) % 3])
          : std::numeric_limits<double>::infinity();
        it = std::min_element(duplicates[dim * 4 + 1].begin(), duplicates[dim * 4 + 1].end(),
          [&](double x, double y) { return std::fabs(x - p12[dim]) < std::fabs(y - p12[dim]); });
        if ((it == duplicates[dim * 4 + 1].end() ||
              (it != duplicates[dim * 4 + 1].end() && std::fabs(*it - p12[dim]) > TOL)) &&
          ((p12[dim] >= boxBounds[2 * dim] && p12[dim] <= boxBounds[2 * dim + 1]) ||
              (vtkMathUtilities::NearlyEqual(p12[dim], boxBounds[2 * dim]) &&
                vtkMathUtilities::NearlyEqual(p12[dim], boxBounds[2 * dim + 1]))) &&
          vtkPolygon::PointInPolygon(p12, faceSize, facePoints.data(), cellBounds, normal))
        {
          volume += (normal[dim] < 0.0 ? 1.0 : -1.0) * vtkMath::Dot(p12, edgeBoxBound1) *
            vtkMath::Dot(p12, edgeNormalBoxBound1) * vtkMath::Dot(p12, normal);
          vtkMath::Cross(edgeBoxBound1, identity + (3 * (dim + 1)) % 9, edgeNormalOnBox1);
          volume += (edgeBoxBound1[(dim + 2) % 3] > 0.0 ? 1.0 : -1.0) *
            vtkMath::Dot(p12, edgeBoxBound1) * p12[(dim + 1) % 3] *
            vtkMath::Dot(p12, edgeNormalOnBox1);
          volume += (normal[dim] > 0.0 ? 1.0 : -1.0) * vtkMath::Dot(p12, edgeBoxBound2) *
            vtkMath::Dot(p12, edgeNormalBoxBound2) * vtkMath::Dot(p12, normal);
          vtkMath::Cross(edgeBoxBound2, identity + (3 * (dim + 2)) % 9, edgeNormalOnBox2);
          volume -= (edgeBoxBound2[(dim + 1) % 3] < 0.0 ? 1.0 : -1.0) *
            vtkMath::Dot(p12, edgeBoxBound2) * p12[(dim + 2) % 3] *
            vtkMath::Dot(p12, edgeNormalOnBox2);
          volume -= (normal[dim] > 0.0 ? 2.0 : -2.0) * p12[0] * p12[1] * p12[2];
        }
        duplicates[dim * 4 + 1].emplace(p12[dim]);

        //  ____v
        // |    |<
        // |    |
        // |____|
        //
        p12[(dim + 2) % 3] = boxBounds[(2 * (dim + 2) + 1) % 6];
        p12[dim] = std::abs(normal[dim]) >= VTK_DBL_EPSILON
          ? -1.0 / normal[dim] * (d + p12[(dim + 1) % 3] * normal[(dim + 1) % 3] +
                                   p12[(dim + 2) % 3] * normal[(dim + 2) % 3])
          : std::numeric_limits<double>::infinity();
        it = std::min_element(duplicates[dim * 4 + 2].begin(), duplicates[dim * 4 + 2].end(),
          [&](double x, double y) { return std::fabs(x - p12[dim]) < std::fabs(y - p12[dim]); });
        if ((it == duplicates[dim * 4 + 2].end() ||
              (it != duplicates[dim * 4 + 2].end() && std::fabs(*it - p12[dim]) > TOL)) &&
          ((p12[dim] >= boxBounds[2 * dim] && p12[dim] <= boxBounds[2 * dim + 1]) ||
              (vtkMathUtilities::NearlyEqual(p12[dim], boxBounds[2 * dim]) &&
                vtkMathUtilities::NearlyEqual(p12[dim], boxBounds[2 * dim + 1]))) &&
          vtkPolygon::PointInPolygon(p12, faceSize, facePoints.data(), cellBounds, normal))
        {
          volume += (normal[dim] > 0.0 ? 1.0 : -1.0) * vtkMath::Dot(p12, edgeBoxBound1) *
            vtkMath::Dot(p12, edgeNormalBoxBound1) * vtkMath::Dot(p12, normal);
          vtkMath::Cross(edgeBoxBound1, identity + (3 * (dim + 1)) % 9, edgeNormalOnBox1);
          volume += (edgeBoxBound1[(dim + 2) % 3] < 0.0 ? 1.0 : -1.0) *
            vtkMath::Dot(p12, edgeBoxBound1) * p12[(dim + 1) % 3] *
            vtkMath::Dot(p12, edgeNormalOnBox1);
          volume += (normal[dim] < 0.0 ? 1.0 : -1.0) * vtkMath::Dot(p12, edgeBoxBound2) *
            vtkMath::Dot(p12, edgeNormalBoxBound2) * vtkMath::Dot(p12, normal);
          vtkMath::Cross(edgeBoxBound2, identity + (3 * (dim + 2)) % 9, edgeNormalOnBox2);
          volume += (edgeBoxBound2[(dim + 1) % 3] < 0.0 ? 1.0 : -1.0) *
            vtkMath::Dot(p12, edgeBoxBound2) * p12[(dim + 2) % 3] *
            vtkMath::Dot(p12, edgeNormalOnBox2);
          volume += (normal[dim] > 0.0 ? 2.0 : -2.0) * p12[0] * p12[1] * p12[2];
        }
        duplicates[dim * 4 + 2].emplace(p12[dim]);

        // v____
        //>|    |
        // |    |
        // |____|
        //
        p12[(dim + 1) % 3] = boxBounds[(2 * (dim + 1)) % 6];
        p12[dim] = std::abs(normal[dim]) >= VTK_DBL_EPSILON
          ? -1.0 / normal[dim] * (d + p12[(dim + 1) % 3] * normal[(dim + 1) % 3] +
                                   p12[(dim + 2) % 3] * normal[(dim + 2) % 3])
          : std::numeric_limits<double>::infinity();
        it = std::min_element(duplicates[dim * 4 + 3].begin(), duplicates[dim * 4 + 3].end(),
          [&](double x, double y) { return std::fabs(x - p12[dim]) < std::fabs(y - p12[dim]); });
        if ((it == duplicates[dim * 4 + 3].end() ||
              (it != duplicates[dim * 4 + 3].end() && std::fabs(*it - p12[dim]) > TOL)) &&
          ((p12[dim] >= boxBounds[2 * dim] && p12[dim] <= boxBounds[2 * dim + 1]) ||
              (vtkMathUtilities::NearlyEqual(p12[dim], boxBounds[2 * dim]) &&
                vtkMathUtilities::NearlyEqual(p12[dim], boxBounds[2 * dim + 1]))) &&
          vtkPolygon::PointInPolygon(p12, faceSize, facePoints.data(), cellBounds, normal))
        {
          volume += (normal[dim] < 0.0 ? 1.0 : -1.0) * vtkMath::Dot(p12, edgeBoxBound1) *
            vtkMath::Dot(p12, edgeNormalBoxBound1) * vtkMath::Dot(p12, normal);
          vtkMath::Cross(edgeBoxBound1, identity + (3 * (dim + 1)) % 9, edgeNormalOnBox1);
          volume -= (edgeBoxBound1[(dim + 2) % 3] < 0.0 ? 1.0 : -1.0) *
            vtkMath::Dot(p12, edgeBoxBound1) * p12[(dim + 1) % 3] *
            vtkMath::Dot(p12, edgeNormalOnBox1);
          volume += (normal[dim] > 0.0 ? 1.0 : -1.0) * vtkMath::Dot(p12, edgeBoxBound2) *
            vtkMath::Dot(p12, edgeNormalBoxBound2) * vtkMath::Dot(p12, normal);
          vtkMath::Cross(edgeBoxBound2, identity + (3 * (dim + 2)) % 9, edgeNormalOnBox2);
          volume += (edgeBoxBound2[(dim + 1) % 3] > 0.0 ? 1.0 : -1.0) *
            vtkMath::Dot(p12, edgeBoxBound2) * p12[(dim + 2) % 3] *
            vtkMath::Dot(p12, edgeNormalOnBox2);
          volume -= (normal[dim] > 0.0 ? 2.0 : -2.0) * p12[0] * p12[1] * p12[2];
        }
        duplicates[dim * 4 + 3].emplace(p12[dim]);
      }
    }
  }
  if (cell3D->IsInsideOut())
  {
    volume = -volume;
  }
  volume += boxVolume;
  volume /= 6.0;
  if (std::abs(volume) >
    (boxBounds[1] - boxBounds[0]) * (boxBounds[3] - boxBounds[2]) * (boxBounds[5] - boxBounds[4]))
  {
    vtkWarningMacro(<< "Something wrong in the computation the intersected volume between a node "
                       "and a cell, returning 0");
    volume = 0.0;
    return false;
  }
  return volume >= VTK_DBL_EPSILON;
}

//----------------------------------------------------------------------------
void vtkResampleToHyperTreeGrid::CreateGridOfMultiResolutionGrids(
  vtkDataSet* dataSet, vtkDataArray* data, int fieldAssociation)
{
  double* bounds = dataSet->GetBounds();

  // Creating the grid of multi resolution grids
  this->GridOfMultiResolutionGrids.resize(
    this->CellDims[0] * this->CellDims[1] * this->CellDims[2]);
  for (std::size_t multiResGridIdx = 0; multiResGridIdx < this->GridOfMultiResolutionGrids.size();
       ++multiResGridIdx)
  {
    this->GridOfMultiResolutionGrids[multiResGridIdx].resize(this->MaxDepth + 1);
  }

  // First pass, we fill the highest resolution grid with input values
  if (fieldAssociation == vtkDataObject::FIELD_ASSOCIATION_POINTS)
  {
    for (vtkIdType pointId = 0; pointId < dataSet->GetNumberOfPoints(); ++pointId)
    {
      double* point = dataSet->GetPoint(pointId);

      // (i, j, k) are the coordinates of the corresponding hyper tree
      vtkIdType i = static_cast<vtkIdType>(((point[0] - bounds[0]) / (bounds[1] - bounds[0]) *
                                             this->CellDims[0] * this->MaxResolutionPerTree) *
                  (1.0 - VTK_DBL_EPSILON)),
                j = static_cast<vtkIdType>(((point[1] - bounds[2]) / (bounds[3] - bounds[2]) *
                                             this->CellDims[1] * this->MaxResolutionPerTree) *
                  (1.0 - VTK_DBL_EPSILON)),
                k = static_cast<vtkIdType>(((point[2] - bounds[4]) / (bounds[5] - bounds[4]) *
                                             this->CellDims[2] * this->MaxResolutionPerTree) *
                  (1.0 - VTK_DBL_EPSILON));

      // We bijectively convert the local coordinates within a hyper tree grid to an integer to pass
      // it
      // to the std::unordered_map at highest resolution
      vtkIdType idx = this->MultiResGridCoordinatesToIndex(i % this->MaxResolutionPerTree,
        j % this->MaxResolutionPerTree, k % this->MaxResolutionPerTree, this->MaxDepth);

      auto& grid =
        this
          ->GridOfMultiResolutionGrids[this->GridCoordinatesToIndex(i / this->MaxResolutionPerTree,
            j / this->MaxResolutionPerTree, k / this->MaxResolutionPerTree)][this->MaxDepth];

      auto it = grid.find(idx);
      // if this is the first time we pass by this grid location, we create a new ArrayMeasurement
      // instance
      // NOTE: GridElement::CanSubdivide does not need to be set at the highest resolution
      if (it == grid.end())
      {
        GridElement& element = grid[idx];
        element.NumberOfLeavesInSubtree = 1;
        element.NumberOfPointsInSubtree = 1;
        element.AccumulatedWeight = 1.0;
        element.UnmaskedChildrenHaveNoMaskedLeaves = true;
        element.Accumulators.resize(this->Accumulators.size());
        for (std::size_t l = 0; l < this->Accumulators.size(); ++l)
        {
          element.Accumulators[l] = this->Accumulators[l]->NewInstance();
          element.Accumulators[l]->DeepCopy(this->Accumulators[l]);
          element.Accumulators[l]->Add(data->GetTuple(pointId), data->GetNumberOfComponents());
        }
      }
      // if not, then the grid location is already created, just need to add the element into it
      else
      {
        for (auto& accumulator : it->second.Accumulators)
        {
          accumulator->Add(data->GetTuple(pointId), data->GetNumberOfComponents());
        }
        ++(it->second.NumberOfPointsInSubtree);
        ++(it->second.AccumulatedWeight);
      }
    }
  }
  else if (fieldAssociation == vtkDataObject::FIELD_ASSOCIATION_CELLS)
  {
    // We allocate weights which are needed to compute the distance between a point and a cell.
    vtkIdType maxNumberOfPoints = 0;
    for (vtkIdType cellId = 0; cellId < dataSet->GetNumberOfCells(); ++cellId)
    {
      vtkCell* cell = dataSet->GetCell(cellId);
      maxNumberOfPoints = maxNumberOfPoints > cell->GetNumberOfPoints() ? maxNumberOfPoints
                                                                        : cell->GetNumberOfPoints();
    }

    // We allocate those variables to avoid unnecessary allocation inside the recursive function.
    // Those are used to check the distance between a point and the cell.
    double* weights = new double[maxNumberOfPoints];

    double volumeUnit = 1.0;
    for (vtkIdType cellId = 0; cellId < dataSet->GetNumberOfCells(); ++cellId)
    {
      vtkCell* cell = dataSet->GetCell(cellId);
      double* cellBounds = cell->GetBounds();
      std::size_t depth = static_cast<std::size_t>(~0);
      vtkIdType imin, imax, jmin, jmax, kmin, kmax;
      do
      {
        ++depth;
        imin = static_cast<vtkIdType>((cellBounds[0] - bounds[0]) * this->ResolutionPerTree[depth] *
          this->CellDims[0] / (bounds[1] - bounds[0]));
        imax =
          static_cast<vtkIdType>(((cellBounds[1] - bounds[0]) * this->ResolutionPerTree[depth] *
                                   this->CellDims[0] / (bounds[1] - bounds[0])) *
            (1.0 - VTK_DBL_EPSILON));
        jmin = static_cast<vtkIdType>((cellBounds[2] - bounds[2]) * this->ResolutionPerTree[depth] *
          this->CellDims[1] / (bounds[3] - bounds[2]));
        jmax =
          static_cast<vtkIdType>(((cellBounds[3] - bounds[2]) * this->ResolutionPerTree[depth] *
                                   this->CellDims[1] / (bounds[3] - bounds[2])) *
            (1.0 - VTK_DBL_EPSILON));
        kmin = static_cast<vtkIdType>((cellBounds[4] - bounds[4]) * this->ResolutionPerTree[depth] *
          this->CellDims[2] / (bounds[5] - bounds[4]));
        kmax =
          static_cast<vtkIdType>(((cellBounds[5] - bounds[4]) * this->ResolutionPerTree[depth] *
                                   this->CellDims[2] / (bounds[5] - bounds[4])) *
            (1.0 - VTK_DBL_EPSILON));
      } while ((imin == imax || jmin == jmax || kmin == kmax) && depth != this->MaxDepth);

      vtkIdType igridmin = imin / this->ResolutionPerTree[depth],
                igridmax = imax / this->ResolutionPerTree[depth],
                jgridmin = jmin / this->ResolutionPerTree[depth],
                jgridmax = jmax / this->ResolutionPerTree[depth],
                kgridmin = kmin / this->ResolutionPerTree[depth],
                kgridmax = kmax / this->ResolutionPerTree[depth];

      for (vtkIdType igrid = igridmin; igrid <= igridmax; ++igrid)
      {
        for (vtkIdType jgrid = jgridmin; jgrid <= jgridmax; ++jgrid)
        {
          for (vtkIdType kgrid = kgridmin; kgrid <= kgridmax; ++kgrid)
          {
            auto& grid =
              this->GridOfMultiResolutionGrids[this->GridCoordinatesToIndex(igrid, jgrid, kgrid)]
                                              [depth];

            for (vtkIdType ii = (igrid == igridmin ? imin % this->ResolutionPerTree[depth] : 0);
                 ii <= (igrid == igridmax ? imax % this->ResolutionPerTree[depth]
                                          : this->ResolutionPerTree[depth] - 1);
                 ++ii)
            {
              for (vtkIdType jj = (jgrid == jgridmin ? jmin % this->ResolutionPerTree[depth] : 0);
                   jj <= (jgrid == jgridmax ? jmax % this->ResolutionPerTree[depth]
                                            : this->ResolutionPerTree[depth] - 1);
                   ++jj)
              {
                for (vtkIdType kk = (kgrid == kgridmin ? kmin % this->ResolutionPerTree[depth] : 0);
                     kk <= (kgrid == kgridmax ? kmax % this->ResolutionPerTree[depth]
                                              : this->ResolutionPerTree[depth] - 1);
                     ++kk)
                {
                  vtkIdType ires = ii + igrid * this->ResolutionPerTree[depth];
                  vtkIdType jres = jj + jgrid * this->ResolutionPerTree[depth];
                  vtkIdType kres = kk + kgrid * this->ResolutionPerTree[depth];

                  double boxBounds[6] = { bounds[0] +
                      (0.0 + ires) / (this->CellDims[0] * this->ResolutionPerTree[depth]) *
                        (bounds[1] - bounds[0]),
                    bounds[0] +
                      (1.0 + ires) / (this->CellDims[0] * this->ResolutionPerTree[depth]) *
                        (bounds[1] - bounds[0]),
                    bounds[2] +
                      (0.0 + jres) / (this->CellDims[1] * this->ResolutionPerTree[depth]) *
                        (bounds[3] - bounds[2]),
                    bounds[2] +
                      (1.0 + jres) / (this->CellDims[1] * this->ResolutionPerTree[depth]) *
                        (bounds[3] - bounds[2]),
                    bounds[4] +
                      (0.0 + kres) / (this->CellDims[2] * this->ResolutionPerTree[depth]) *
                        (bounds[5] - bounds[4]),
                    bounds[4] +
                      (1.0 + kres) / (this->CellDims[2] * this->ResolutionPerTree[depth]) *
                        (bounds[5] - bounds[4]) };

                  double volume = 0.0;
                  bool nonZeroVolume = false;

                  vtkCell3D* cell3D = vtkCell3D::SafeDownCast(cell);
                  vtkVoxel* voxel = vtkVoxel::SafeDownCast(cell);

                  if (voxel)
                  {
                    nonZeroVolume = this->IntersectedVolume(boxBounds, voxel, volumeUnit, volume);
                  }
                  else if (cell3D)
                  {
                    nonZeroVolume =
                      this->IntersectedVolume(boxBounds, cell3D, volumeUnit, volume, weights);
                  }
                  else
                  {
                    vtkErrorMacro(<< "cell type " << cell->GetClassName() << " not supported");
                  }

                  if (nonZeroVolume)
                  {
                    vtkIdType gridIdx = this->MultiResGridCoordinatesToIndex(ii, jj, kk, depth);
                    auto it = grid.find(gridIdx);
                    if (it == grid.end())
                    {
                      GridElement& element = grid[gridIdx];
                      element.NumberOfLeavesInSubtree = 1;
                      element.NumberOfPointsInSubtree = 1;
                      element.UnmaskedChildrenHaveNoMaskedLeaves = true;
                      element.AccumulatedWeight = volume;
                      element.Accumulators.resize(this->Accumulators.size());
                      for (std::size_t l = 0; l < this->Accumulators.size(); ++l)
                      {
                        element.Accumulators[l] = this->Accumulators[l]->NewInstance();
                        element.Accumulators[l]->DeepCopy(this->Accumulators[l]);
                        element.Accumulators[l]->Add(
                          data->GetTuple(cellId), data->GetNumberOfComponents(), volume);
                      }
                    }
                    // if not, then the grid location is already created, just need to add the
                    // element into it
                    else
                    {
                      for (auto& accumulator : it->second.Accumulators)
                      {
                        accumulator->Add(
                          data->GetTuple(cellId), data->GetNumberOfComponents(), volume);
                      }
                      ++(it->second.NumberOfPointsInSubtree);
                      it->second.AccumulatedWeight += volume;
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    delete[] weights;
  }
  else
  {
    vtkWarningMacro(<< "Unknown field association. Supported are points and cells");
  }

  // Now, we fill the multi-resolution grid bottom-up
  for (std::size_t multiResGridIdx = 0; multiResGridIdx < this->GridOfMultiResolutionGrids.size();
       ++multiResGridIdx)
  {
    auto& multiResolutionGrid = this->GridOfMultiResolutionGrids[multiResGridIdx];
    for (std::size_t depth = this->MaxDepth; depth; --depth)
    {
      // The strategy is the following:
      // Given an iterator on the elements of the grid at resolution depth,
      // we propagate the accumulated values to the lower resolution depth-1
      // using correct indexing
      for (const auto& mapElement : multiResolutionGrid[depth])
      {
        vtkTuple<vtkIdType, 3> coord =
          this->IndexToMultiResGridCoordinates(mapElement.first, depth);
        coord[0] /= this->BranchFactor;
        coord[1] /= this->BranchFactor;
        coord[2] /= this->BranchFactor;
        vtkIdType idx =
          this->MultiResGridCoordinatesToIndex(coord[0], coord[1], coord[2], depth - 1);

        // Same as before: if the grid location is not created yet, we create it, if not,
        // we merge the corresponding accumulated values
        auto it = multiResolutionGrid[depth - 1].find(idx);
        // if the grid element does not exist yet, we create it
        if (it == multiResolutionGrid[depth - 1].end())
        {
          GridElement& element = multiResolutionGrid[depth - 1][idx];

          // Initializing element
          element.NumberOfLeavesInSubtree = mapElement.second.NumberOfLeavesInSubtree;
          element.NumberOfPointsInSubtree = mapElement.second.NumberOfPointsInSubtree;
          element.NumberOfNonMaskedChildren = 1;
          element.AccumulatedWeight = mapElement.second.AccumulatedWeight;

          // mapElement, from higher depth, can have no children with any masked leaves,
          // but have a masked children, which we propagate upward.
          element.UnmaskedChildrenHaveNoMaskedLeaves =
            mapElement.second.UnmaskedChildrenHaveNoMaskedLeaves &&
            mapElement.second.NumberOfNonMaskedChildren == this->NumberOfChildren;

          // A leaf can be subivided if each of the hypothetical child:
          // - Has at least MinimumNumberOfPointsInSubtree set by the user
          // - Has enough points to be measured
          // Here we check with the first child.
          element.CanSubdivide =
            mapElement.second.NumberOfPointsInSubtree >= this->MinimumNumberOfPointsInSubtree &&
            (!this->ArrayMeasurement ||
              this->ArrayMeasurement->CanMeasure(
                mapElement.second.NumberOfPointsInSubtree, mapElement.second.AccumulatedWeight)) &&
            (!this->ArrayMeasurementDisplay ||
              this->ArrayMeasurementDisplay->CanMeasure(
                mapElement.second.NumberOfPointsInSubtree, mapElement.second.AccumulatedWeight));

          // We copy the accumulator of the child
          element.Accumulators.resize(this->Accumulators.size());
          for (std::size_t l = 0; l < this->Accumulators.size(); ++l)
          {
            element.Accumulators[l] = this->Accumulators[l]->NewInstance();
            element.Accumulators[l]->DeepCopy(this->Accumulators[l]);
            element.Accumulators[l]->Add(mapElement.second.Accumulators[l]);
          }
        }
        // else, the grid element is already created, we add data to it
        else
        {
          // Adding information from subtree
          it->second.NumberOfLeavesInSubtree += mapElement.second.NumberOfLeavesInSubtree;
          it->second.NumberOfPointsInSubtree += mapElement.second.NumberOfPointsInSubtree;
          it->second.AccumulatedWeight += mapElement.second.AccumulatedWeight;

          // mapElement, from higher depth, can have no children with any masked leaves,
          // but have a masked children, which we propagate upward.
          it->second.UnmaskedChildrenHaveNoMaskedLeaves &=
            mapElement.second.UnmaskedChildrenHaveNoMaskedLeaves &&
            mapElement.second.NumberOfNonMaskedChildren == this->NumberOfChildren;
          ++(it->second.NumberOfNonMaskedChildren);

          // A leaf can be subivided if each of the hypothetical child:
          // - Has at least MinimumNumberOfPointsInSubtree set by the user
          // - Has enough points to be measured
          // Here we accumulate for each child
          it->second.CanSubdivide &=
            it->second.NumberOfPointsInSubtree >= this->MinimumNumberOfPointsInSubtree &&
            (!this->ArrayMeasurement ||
              this->ArrayMeasurement->CanMeasure(
                mapElement.second.NumberOfPointsInSubtree, mapElement.second.AccumulatedWeight)) &&
            (!this->ArrayMeasurementDisplay ||
              this->ArrayMeasurementDisplay->CanMeasure(
                mapElement.second.NumberOfPointsInSubtree, mapElement.second.AccumulatedWeight));

          // We add the accumulators from the child
          for (std::size_t l = 0; l < this->Accumulators.size(); ++l)
          {
            it->second.Accumulators[l]->Add(mapElement.second.Accumulators[l]);
          }
        }
      }
    }
  }

  if (this->NoEmptyCells ||
    (this->Extrapolate && fieldAssociation == vtkDataObject::FIELD_ASSOCIATION_POINTS))
  {
    // We allocate weights which are needed to compute the distance between a point and a cell.
    vtkIdType maxNumberOfPoints = 0;
    for (vtkIdType cellId = 0; cellId < dataSet->GetNumberOfCells(); ++cellId)
    {
      vtkCell* cell = dataSet->GetCell(cellId);
      maxNumberOfPoints = maxNumberOfPoints > cell->GetNumberOfPoints() ? maxNumberOfPoints
                                                                        : cell->GetNumberOfPoints();
    }

    // We allocate those variables to avoid unnecessary allocation inside the recursive function.
    // Those are used to check the distance between a point and the cell.
    double x[3], pcoords[3], closestPoint[3];
    double* weights = new double[maxNumberOfPoints];

    // We forbid subdividing if a child is masked and has geometry passing through it.
    for (vtkIdType cellId = 0; cellId < dataSet->GetNumberOfCells(); ++cellId)
    {
      this->UpdateProgress((double)cellId / dataSet->GetNumberOfCells());

      // The strategy is the following:
      // We go through all the coordinates in the multi resolution grid
      // that intersect the bounding box of the input cell.
      // Then we check if the corresponding position in near enough to the cell.
      // If it it, we forbid subdivision with GridElement::CanSubdivide
      vtkCell* cell = dataSet->GetCell(cellId);
      double* cellBounds = cell->GetBounds();
      vtkIdType imin = static_cast<vtkIdType>(
                  (cellBounds[0] - bounds[0]) * this->CellDims[0] / (bounds[1] - bounds[0])),
                imax = static_cast<vtkIdType>(
                  ((cellBounds[1] - bounds[0]) * this->CellDims[0] / (bounds[1] - bounds[0])) *
                  (1.0 - VTK_DBL_EPSILON)),
                jmin = static_cast<vtkIdType>(
                  (cellBounds[2] - bounds[2]) * this->CellDims[1] / (bounds[3] - bounds[2])),
                jmax = static_cast<vtkIdType>(
                  ((cellBounds[3] - bounds[2]) * this->CellDims[1] / (bounds[3] - bounds[2])) *
                  (1.0 - VTK_DBL_EPSILON)),
                kmin = static_cast<vtkIdType>(
                  (cellBounds[4] - bounds[4]) * this->CellDims[2] / (bounds[5] - bounds[4])),
                kmax = static_cast<vtkIdType>(
                  ((cellBounds[5] - bounds[4]) * this->CellDims[2] / (bounds[5] - bounds[4])) *
                  (1.0 - VTK_DBL_EPSILON));

      // For each hyper tree intersecting the bounding box
      for (vtkIdType i = imin; i <= imax; ++i)
      {
        for (vtkIdType j = jmin; j <= jmax; ++j)
        {
          for (vtkIdType k = kmin; k <= kmax; ++k)
          {
            this->RecursivelyFillGaps(cell, bounds, cellBounds, i, j, k, x, closestPoint, pcoords,
              weights,
              this->Extrapolate && fieldAssociation == vtkDataObject::FIELD_ASSOCIATION_POINTS);
          }
        }
      }
    }
    delete[] weights;
  }
}

//----------------------------------------------------------------------------
bool vtkResampleToHyperTreeGrid::RecursivelyFillGaps(vtkCell* cell, const double bounds[6],
  const double cellBounds[6], vtkIdType i, vtkIdType j, vtkIdType k, double x[3],
  double closestPoint[3], double pcoords[3], double* weights, bool markEmpty, vtkIdType ii,
  vtkIdType jj, vtkIdType kk, std::size_t depth)
{
  assert(depth <= this->MaxDepth && "Too deep");

  vtkIdType idx = this->MultiResGridCoordinatesToIndex(ii, jj, kk, depth);
  std::size_t multiResGridIdx = this->GridCoordinatesToIndex(i, j, k);
  auto it = this->GridOfMultiResolutionGrids[multiResGridIdx][depth].find(idx);

  // We are only interested by masked grid positions, i.e. uncreated position in the
  // std::unordered_map.
  if (it == this->GridOfMultiResolutionGrids[multiResGridIdx][depth].end())
  {
    int subId;
    double dist2;

    // x is the center of the grid position
    x[0] = bounds[0] +
      (0.5 + i * this->ResolutionPerTree[depth] + ii) /
        (this->CellDims[0] * this->ResolutionPerTree[depth]) * (bounds[1] - bounds[0]);
    x[1] = bounds[2] +
      (0.5 + j * this->ResolutionPerTree[depth] + jj) /
        (this->CellDims[1] * this->ResolutionPerTree[depth]) * (bounds[3] - bounds[2]);
    x[2] = bounds[4] +
      (0.5 + k * this->ResolutionPerTree[depth] + kk) /
        (this->CellDims[2] * this->ResolutionPerTree[depth]) * (bounds[5] - bounds[4]);

    int inside = cell->EvaluatePosition(x, closestPoint, subId, pcoords, dist2, weights);
    bool result = inside != 0;
    if (markEmpty && result)
    {
      // There is geometry, we create empty element at index idx
      this->GridOfMultiResolutionGrids[multiResGridIdx][depth][idx];
    }
    // We tell the parent if its empty child has geometry in it.
    return result;
  }

  // No need to continue if we are deep enough or if we already cannot subdivide / have full
  // subtree.
  if (depth == MaxDepth || !it->second.CanSubdivide ||
    (it->second.NumberOfNonMaskedChildren == this->NumberOfChildren &&
        it->second.UnmaskedChildrenHaveNoMaskedLeaves))
  {
    return true;
  }

  // We recurse into each grid position at a deeper level intersecting the cell bounding box.
  for (vtkIdType iii = 0; iii < this->BranchFactor; ++iii)
  {
    double xmin = bounds[0] +
      (0.0 + i * this->ResolutionPerTree[depth + 1] + ii * this->BranchFactor + iii) /
        (this->CellDims[0] * this->ResolutionPerTree[depth + 1]) * (bounds[1] - bounds[0]),
           xmax = bounds[0] +
      (1.0 + i * this->ResolutionPerTree[depth + 1] + ii * this->BranchFactor + iii) /
        (this->CellDims[0] * this->ResolutionPerTree[depth + 1]) * (bounds[1] - bounds[0]);

    for (vtkIdType jjj = 0; jjj < this->BranchFactor; ++jjj)
    {
      double ymin = bounds[2] +
        (0.0 + j * this->ResolutionPerTree[depth + 1] + jj * this->BranchFactor + jjj) /
          (this->CellDims[1] * this->ResolutionPerTree[depth + 1]) * (bounds[3] - bounds[2]),
             ymax = bounds[2] +
        (1.0 + j * this->ResolutionPerTree[depth + 1] + jj * this->BranchFactor + jjj) /
          (this->CellDims[1] * this->ResolutionPerTree[depth + 1]) * (bounds[3] - bounds[2]);

      for (vtkIdType kkk = 0; kkk < this->BranchFactor; ++kkk)
      {
        double zmin = bounds[4] +
          (0.0 + k * this->ResolutionPerTree[depth + 1] + kk * this->BranchFactor + kkk) /
            (this->CellDims[2] * this->ResolutionPerTree[depth + 1]) * (bounds[5] - bounds[4]),
               zmax = bounds[4] +
          (1.0 + k * this->ResolutionPerTree[depth + 1] + kk * this->BranchFactor + kkk) /
            (this->CellDims[2] * this->ResolutionPerTree[depth + 1]) * (bounds[5] - bounds[4]);

        // if child intersects the cell bounding box
        if (xmin <= cellBounds[1] && xmax >= cellBounds[0] && ymin <= cellBounds[3] &&
          ymax >= cellBounds[2] && zmin <= cellBounds[5] && zmax >= cellBounds[4])
        {
          if (markEmpty)
          {
            this->RecursivelyFillGaps(cell, bounds, cellBounds, i, j, k, x, closestPoint, pcoords,
              weights, markEmpty, ii * this->BranchFactor + iii, jj * this->BranchFactor + jjj,
              kk * this->BranchFactor + kkk, depth + 1);
          }
          else
          {
            // We ask this child if it is ok to subdivide.
            it->second.CanSubdivide &= this->RecursivelyFillGaps(cell, bounds, cellBounds, i, j, k,
              x, closestPoint, pcoords, weights, markEmpty, ii * this->BranchFactor + iii,
              jj * this->BranchFactor + jjj, kk * this->BranchFactor + kkk, depth + 1);
          }
        }
      }
    }
  }
  return true;
}

//----------------------------------------------------------------------------
void vtkResampleToHyperTreeGrid::ExtrapolateValuesOnGaps(vtkHyperTreeGrid* htg)
{
  vtkHyperTreeGrid::vtkHyperTreeGridIterator it;
  htg->InitializeTreeIterator(it);
  vtkIdType treeId;
  PriorityQueue pq, pqtmp;
  while (it.GetNextTree(treeId))
  {
    vtkNew<vtkHyperTreeGridNonOrientedVonNeumannSuperCursor> superCursor;
    superCursor->Initialize(htg, treeId);
    this->RecursivelyFillPriorityQueue(superCursor, pq);
  }
  std::vector<PriorityQueueElement> buf;
  while (pq.size())
  {
    const PriorityQueueElement& qe = pq.top();
    vtkIdType id = qe.Id, key = qe.Key;
    double mean = qe.Mean, displayMean = qe.DisplayMean;
    vtkIdType invalidNeighbors = 0;
    for (std::size_t i = 0; i < qe.InvalidNeighborIds.size(); ++i)
    {
      double value = this->ScalarField->GetValue(qe.InvalidNeighborIds[i]);
      if (value == value)
      {
        mean += value;
        if (this->DisplayScalarField)
        {
          displayMean += this->DisplayScalarField->GetValue(qe.InvalidNeighborIds[i]);
        }
      }
      else
      {
        ++invalidNeighbors;
      }
    }
    buf.emplace_back(PriorityQueueElement(
      qe.Key + static_cast<vtkIdType>(qe.InvalidNeighborIds.size()) - invalidNeighbors, id, mean,
      displayMean));
    pq.pop();
    if (!pq.size() || pq.top().Key != key)
    {
      for (const PriorityQueueElement& element : buf)
      {
        this->ScalarField->SetValue(element.Id, element.Mean / element.Key);
        if (this->DisplayScalarField)
        {
          this->DisplayScalarField->SetValue(element.Id, element.DisplayMean / element.Key);
        }
      }
      buf.clear();
    }
  }
}

//----------------------------------------------------------------------------
void vtkResampleToHyperTreeGrid::RecursivelyFillPriorityQueue(
  vtkHyperTreeGridNonOrientedVonNeumannSuperCursor* superCursor, PriorityQueue& pq)
{
  vtkIdType superCursorId = superCursor->GetGlobalNodeIndex();
  double value = this->ScalarField->GetValue(superCursorId);
  if (value != value)
  {
    PriorityQueueElement qe;
    vtkIdType numberOfCursors = superCursor->GetNumberOfCursors();
    vtkIdType validNeighbors = 0;
    for (vtkIdType iCursor = 0; iCursor < numberOfCursors; ++iCursor)
    {
      vtkIdType id = superCursor->GetGlobalNodeIndex(iCursor);

      if (id != vtkHyperTreeGrid::InvalidIndex && !superCursor->IsMasked(iCursor))
      {
        value = this->ScalarField->GetValue(id);
        if (value != value)
        {
          qe.InvalidNeighborIds.push_back(id);
        }
        else
        {
          ++validNeighbors;
          qe.Mean += value;
          if (this->DisplayScalarField)
          {
            qe.DisplayMean += this->DisplayScalarField->GetValue(id);
          }
        }
      }
    }
    if (!qe.InvalidNeighborIds.size())
    {
      this->ScalarField->SetValue(superCursorId, qe.Mean / validNeighbors);
      if (this->DisplayScalarField)
      {
        this->ScalarField->SetValue(superCursorId, validNeighbors);
      }
    }
    else
    {
      qe.Id = superCursorId;
      qe.Key = validNeighbors;
      pq.emplace(std::move(qe));
    }
  }
  else if (!superCursor->IsLeaf())
  {
    vtkIdType numberOfChildren = superCursor->GetNumberOfChildren();
    for (vtkIdType ichild = 0; ichild < numberOfChildren; ++ichild)
    {
      superCursor->ToChild(ichild);
      this->RecursivelyFillPriorityQueue(superCursor, pq);
      superCursor->ToParent();
    }
  }
}

//----------------------------------------------------------------------------
int vtkResampleToHyperTreeGrid::GenerateTrees(vtkHyperTreeGrid* htg)
{
  // Iterate over all hyper trees
  this->Progress = 0.;

  vtkIdType treeOffset = 0;

  vtkIdType multiResGridIdx = 0;
  for (vtkIdType i = 0; i < htg->GetCellDims()[0]; ++i)
  {
    for (vtkIdType j = 0; j < htg->GetCellDims()[1]; ++j)
    {
      for (vtkIdType k = 0; k < htg->GetCellDims()[2]; ++k, ++multiResGridIdx)
      {
        vtkIdType treeId;
        htg->GetIndexFromLevelZeroCoordinates(treeId, i, j, k);
        // Build this tree:
        vtkHyperTreeGridNonOrientedCursor* cursor = htg->NewNonOrientedCursor(treeId, true);
        cursor->GetTree()->SetGlobalIndexStart(treeOffset);
        // We subdivide each tree starting at position (0,0,0) at coarsest level
        // We feed the corresponding multi resolution grid
        // Top-down algorithm
        this->SubdivideLeaves(
          cursor, treeId, 0, 0, 0, this->GridOfMultiResolutionGrids[multiResGridIdx]);
        treeOffset += cursor->GetTree()->GetNumberOfVertices();
        cursor->FastDelete();
      }
    }
  }

  return 1;
}

//----------------------------------------------------------------------------
void vtkResampleToHyperTreeGrid::SubdivideLeaves(vtkHyperTreeGridNonOrientedCursor* cursor,
  vtkIdType treeId, vtkIdType i, vtkIdType j, vtkIdType k, MultiResGridType& multiResolutionGrid)
{
  vtkIdType level = cursor->GetLevel();
  vtkIdType vertexId = cursor->GetVertexId();
  vtkHyperTree* tree = cursor->GetTree();
  vtkIdType idx = tree->GetGlobalIndexFromLocal(vertexId);

  auto it = multiResolutionGrid[level].find(this->MultiResGridCoordinatesToIndex(i, j, k, level));
  double value = 0, valueDisplay = 0;

  if (it != multiResolutionGrid[level].end())
  {
    if (it->second.Accumulators.size())
    {
      // If we use this->ArrayMeasurementDisplay, we need to put the right accumulators
      // in the right place and then measure
      if (this->ArrayMeasurementDisplay)
      {
        for (std::size_t l = 0; l < this->ArrayMeasurementAccumulators.size(); ++l)
        {
          this->ArrayMeasurementAccumulators[l] = it->second.Accumulators[l];
        }
        if (this->ArrayMeasurement)
        {
          this->ArrayMeasurement->Measure(this->ArrayMeasurementAccumulators.data(),
            it->second.NumberOfPointsInSubtree, it->second.AccumulatedWeight, value);
        }

        for (std::size_t l = 0; l < this->ArrayMeasurementDisplayAccumulators.size(); ++l)
        {
          this->ArrayMeasurementDisplayAccumulators[l] =
            it->second.Accumulators[this->ArrayMeasurementDisplayAccumulatorMap[l]];
        }
        this->ArrayMeasurementDisplay->Measure(this->ArrayMeasurementDisplayAccumulators.data(),
          it->second.NumberOfPointsInSubtree, it->second.AccumulatedWeight, valueDisplay);
      }
      // Else, we just measure
      else
      {
        if (this->ArrayMeasurement)
        {
          this->ArrayMeasurement->Measure(it->second.Accumulators.data(),
            it->second.NumberOfPointsInSubtree, it->second.AccumulatedWeight, value);
        }
      }
    }
    else
    {
      value = std::numeric_limits<double>::quiet_NaN();
      valueDisplay = std::numeric_limits<double>::quiet_NaN();
    }
  }

  if (this->ArrayMeasurement)
  {
    this->ScalarField->InsertValue(idx, value);
  }
  if (this->ArrayMeasurementDisplay)
  {
    this->DisplayScalarField->InsertValue(idx, valueDisplay);
  }
  this->NumberOfLeavesInSubtreeField->InsertValue(
    idx, it != multiResolutionGrid[level].end() ? it->second.NumberOfLeavesInSubtree : 0);
  this->NumberOfPointsInSubtreeField->InsertValue(
    idx, it != multiResolutionGrid[level].end() ? it->second.NumberOfPointsInSubtree : 0);
  this->Mask->InsertValue(idx, it == multiResolutionGrid[level].end());

  if (cursor->IsLeaf())
  {
    // If we match the criterion, we subdivide
    // Also: if the subtrees have only one element, it is useless to subdivide, we already are at
    // the finest possible resolution given input data
    if (level <= this->MaxDepth && it != multiResolutionGrid[level].end() && value == value &&
      it->second.NumberOfLeavesInSubtree > 1 && it->second.CanSubdivide &&
      (!this->ArrayMeasurement || (this->InRange && value > this->Min && value < this->Max) ||
          (!this->InRange && !(value > this->Min && value < this->Max))))
    {
      cursor->SubdivideLeaf();
    }
    else
    {
      return;
    }
  }

  // We iterate in the neighborhood and zoom into higher resolution level
  int ii = 0, jj = 0, kk = 0;
  for (int childIdx = 0; childIdx < cursor->GetNumberOfChildren(); ++childIdx)
  {
    cursor->ToChild(childIdx);
    this->SubdivideLeaves(cursor, treeId, i * tree->GetBranchFactor() + ii,
      j * tree->GetBranchFactor() + jj, k * tree->GetBranchFactor() + kk, multiResolutionGrid);
    cursor->ToParent();

    if (++ii == tree->GetBranchFactor())
    {
      if (++jj == tree->GetBranchFactor())
      {
        ++kk;
        jj = 0;
      }
      ii = 0;
    }
  }
}

//----------------------------------------------------------------------------
int vtkResampleToHyperTreeGrid::ProcessRequest(
  vtkInformation* request, vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  // create the output
  if (request->Has(vtkDemandDrivenPipeline::REQUEST_DATA_OBJECT()))
  {
    return this->RequestDataObject(request, inputVector, outputVector);
  }

  // generate the data
  if (request->Has(vtkDemandDrivenPipeline::REQUEST_DATA()))
  {
    return this->RequestData(request, inputVector, outputVector);
  }

  if (request->Has(vtkStreamingDemandDrivenPipeline::REQUEST_UPDATE_EXTENT()))
  {
    return this->RequestUpdateExtent(request, inputVector, outputVector);
  }

  // execute information
  if (request->Has(vtkDemandDrivenPipeline::REQUEST_INFORMATION()))
  {
    return this->RequestInformation(request, inputVector, outputVector);
  }

  return this->Superclass::ProcessRequest(request, inputVector, outputVector);
}

int vtkResampleToHyperTreeGrid::RequestDataObject(
  vtkInformation*, vtkInformationVector** inputVector, vtkInformationVector* outputVector)
{
  if (this->GetNumberOfInputPorts() == 0 || this->GetNumberOfOutputPorts() == 0)
  {
    return 1;
  }

  vtkInformation* inInfo = inputVector[0]->GetInformationObject(0);
  if (!inInfo)
  {
    return 0;
  }
  vtkDataObject* input = inInfo->Get(vtkDataObject::DATA_OBJECT());

  if (input)
  {
    // for each output
    for (int i = 0; i < this->GetNumberOfOutputPorts(); ++i)
    {
      vtkInformation* info = outputVector->GetInformationObject(i);
      vtkDataObject* output = info->Get(vtkDataObject::DATA_OBJECT());

      if (!output || !output->IsA(input->GetClassName()))
      {
        vtkDataObject* newOutput = input->NewInstance();
        info->Set(vtkDataObject::DATA_OBJECT(), newOutput);
        newOutput->Delete();
      }
    }
  }
  return 1;
}

//----------------------------------------------------------------------------
int vtkResampleToHyperTreeGrid::RequestUpdateExtent(
  vtkInformation*, vtkInformationVector** inputVector, vtkInformationVector*)
{
  int numInputPorts = this->GetNumberOfInputPorts();
  for (int i = 0; i < numInputPorts; ++i)
  {
    int numInputConnections = this->GetNumberOfInputConnections(i);
    for (int j = 0; j < numInputConnections; ++j)
    {
      vtkInformation* inputInfo = inputVector[i]->GetInformationObject(j);
      inputInfo->Set(vtkStreamingDemandDrivenPipeline::EXACT_EXTENT(), 1);
    }
  }
  return 1;
}

//----------------------------------------------------------------------------
void vtkResampleToHyperTreeGrid::SetMaxToInfinity()
{
  this->SetMax(std::numeric_limits<double>::infinity());
}

//----------------------------------------------------------------------------
void vtkResampleToHyperTreeGrid::SetMinToInfinity()
{
  this->SetMin(-std::numeric_limits<double>::infinity());
}

//----------------------------------------------------------------------------
vtkTuple<vtkIdType, 3> vtkResampleToHyperTreeGrid::IndexToMultiResGridCoordinates(
  vtkIdType idx, std::size_t depth) const
{
  vtkTuple<vtkIdType, 3> coord;
  coord[2] = idx % (this->ResolutionPerTree[depth]);
  coord[1] = (idx / this->ResolutionPerTree[depth]) % this->ResolutionPerTree[depth];
  coord[0] = idx / (this->ResolutionPerTree[depth] * this->ResolutionPerTree[depth]);
  return coord;
}

//----------------------------------------------------------------------------
vtkTuple<vtkIdType, 3> vtkResampleToHyperTreeGrid::IndexToGridCoordinates(std::size_t idx) const
{
  vtkTuple<vtkIdType, 3> coord;
  coord[2] = idx % (this->CellDims[2]);
  coord[1] = (idx / this->CellDims[2]) % this->CellDims[1];
  coord[0] = idx / (this->ResolutionPerTree[CellDims[2]] * this->CellDims[1]);
  return coord;
}

//----------------------------------------------------------------------------
vtkIdType vtkResampleToHyperTreeGrid::MultiResGridCoordinatesToIndex(
  vtkIdType i, vtkIdType j, vtkIdType k, std::size_t depth) const
{
  return k + j * this->ResolutionPerTree[depth] +
    i * this->ResolutionPerTree[depth] * this->ResolutionPerTree[depth];
}

//----------------------------------------------------------------------------
std::size_t vtkResampleToHyperTreeGrid::GridCoordinatesToIndex(
  vtkIdType i, vtkIdType j, vtkIdType k) const
{
  return k + j * this->CellDims[2] + i * this->CellDims[2] * this->CellDims[1];
}

//----------------------------------------------------------------------------
void vtkResampleToHyperTreeGrid::SetMaxState(bool state)
{
  if (!state)
  {
    if (this->Max == std::numeric_limits<double>::infinity())
    {
      return;
    }
    this->MaxCache = this->Max;
    this->SetMaxToInfinity();
  }
  else
  {
    this->SetMax(std::min(this->MaxCache, this->Max));
  }
}

//----------------------------------------------------------------------------
void vtkResampleToHyperTreeGrid::SetMinState(bool state)
{
  if (!state)
  {
    if (this->Min == -std::numeric_limits<double>::infinity())
    {
      return;
    }
    this->MinCache = this->Min;
    this->SetMinToInfinity();
  }
  else
  {
    this->SetMin(std::max(this->MinCache, this->Min));
  }
}

//----------------------------------------------------------------------------
vtkMTimeType vtkResampleToHyperTreeGrid::GetMTime()
{
  vtkMTimeType time = this->Superclass::GetMTime();
  if (this->ArrayMeasurement)
  {
    time = std::max(this->ArrayMeasurement->GetMTime(), time);
  }
  if (this->ArrayMeasurementDisplay)
  {
    time = std::max(this->ArrayMeasurementDisplay->GetMTime(), time);
  }
  return time;
}
