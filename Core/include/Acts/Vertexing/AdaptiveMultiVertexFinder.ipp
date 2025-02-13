// This file is part of the Acts project.
//
// Copyright (C) 2020 CERN for the benefit of the Acts project
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "Acts/Utilities/AlgebraHelpers.hpp"
#include "Acts/Vertexing/VertexingError.hpp"

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::find(
    const std::vector<const InputTrack_t*>& allTracks,
    const VertexingOptions<InputTrack_t>& vertexingOptions,
    State& /*state*/) const -> Result<std::vector<Vertex<InputTrack_t>>> {
  if (allTracks.empty()) {
    ACTS_ERROR("Empty track collection handed to find method");
    return VertexingError::EmptyInput;
  }
  // Original tracks
  const std::vector<const InputTrack_t*>& origTracks = allTracks;

  // Seed tracks
  std::vector<const InputTrack_t*> seedTracks = allTracks;

  FitterState_t fitterState(*m_cfg.bField, vertexingOptions.magFieldContext);
  SeedFinderState_t seedFinderState;

  std::vector<std::unique_ptr<Vertex<InputTrack_t>>> allVertices;

  std::vector<Vertex<InputTrack_t>*> allVerticesPtr;

  int iteration = 0;
  std::vector<const InputTrack_t*> removedSeedTracks;
  while (((m_cfg.addSingleTrackVertices && !seedTracks.empty()) ||
          ((!m_cfg.addSingleTrackVertices) && !seedTracks.empty())) &&
         iteration < m_cfg.maxIterations) {
    // Tracks that are used for searching compatible tracks
    // near a vertex candidate
    std::vector<const InputTrack_t*> searchTracks;
    if (m_cfg.doRealMultiVertex) {
      searchTracks = origTracks;
    } else {
      searchTracks = seedTracks;
    }
    Vertex<InputTrack_t> currentConstraint = vertexingOptions.constraint;
    // Retrieve seed vertex from all remaining seedTracks
    auto seedResult = doSeeding(seedTracks, currentConstraint, vertexingOptions,
                                seedFinderState, removedSeedTracks);
    if (!seedResult.ok()) {
      return seedResult.error();
    }
    allVertices.push_back(std::make_unique<Vertex<InputTrack_t>>(*seedResult));

    Vertex<InputTrack_t>& vtxCandidate = *allVertices.back();
    allVerticesPtr.push_back(&vtxCandidate);

    ACTS_DEBUG("Position of vertex candidate after seeding: "
               << vtxCandidate.fullPosition().transpose());
    if (vtxCandidate.position().z() ==
        vertexingOptions.constraint.position().z()) {
      ACTS_DEBUG(
          "No seed found anymore. Break and stop primary vertex finding.");
      allVertices.pop_back();
      allVerticesPtr.pop_back();
      break;
    }

    // Clear the seed track collection that has been removed in last iteration
    // now after seed finding is done
    removedSeedTracks.clear();

    auto prepResult = canPrepareVertexForFit(searchTracks, seedTracks,
                                             vtxCandidate, currentConstraint,
                                             fitterState, vertexingOptions);

    if (!prepResult.ok()) {
      return prepResult.error();
    }
    if (!(*prepResult)) {
      ACTS_DEBUG(
          "Could not prepare for fit. Discarding the vertex candindate.");
      allVertices.pop_back();
      allVerticesPtr.pop_back();
      break;
    }
    // Update fitter state with all vertices
    fitterState.addVertexToMultiMap(vtxCandidate);

    // Perform the fit
    auto fitResult = m_cfg.vertexFitter.addVtxToFit(
        fitterState, vtxCandidate, m_cfg.linearizer, vertexingOptions);
    if (!fitResult.ok()) {
      return fitResult.error();
    }
    ACTS_DEBUG("Position of vertex candidate after the fit: "
               << vtxCandidate.fullPosition().transpose());
    // Check if vertex is good vertex
    auto [nCompatibleTracks, isGoodVertex] =
        checkVertexAndCompatibleTracks(vtxCandidate, seedTracks, fitterState,
                                       vertexingOptions.useConstraintInFit);

    ACTS_DEBUG("Vertex is good vertex: " << isGoodVertex);
    if (nCompatibleTracks > 0) {
      removeCompatibleTracksFromSeedTracks(vtxCandidate, seedTracks,
                                           fitterState, removedSeedTracks);
    } else {
      bool removedIncompatibleTrack = removeTrackIfIncompatible(
          vtxCandidate, seedTracks, fitterState, removedSeedTracks,
          vertexingOptions.geoContext);
      if (!removedIncompatibleTrack) {
        ACTS_DEBUG(
            "Could not remove any further track from seed tracks. Break.");
        allVertices.pop_back();
        allVerticesPtr.pop_back();
        break;
      }
    }
    // MARK: fpeMaskBegin(FLTUND, 1, #2590)
    bool keepVertex = isGoodVertex &&
                      keepNewVertex(vtxCandidate, allVerticesPtr, fitterState);
    // MARK: fpeMaskEnd(FLTUND)
    ACTS_DEBUG("New vertex will be saved: " << keepVertex);

    // Delete vertex from allVertices list again if it's not kept
    if (!keepVertex) {
      auto deleteVertexResult =
          deleteLastVertex(vtxCandidate, allVertices, allVerticesPtr,
                           fitterState, vertexingOptions);
      if (!deleteVertexResult.ok()) {
        return deleteVertexResult.error();
      }
    }
    iteration++;
  }  // end while loop

  return getVertexOutputList(allVerticesPtr, fitterState);
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::doSeeding(
    const std::vector<const InputTrack_t*>& trackVector,
    Vertex<InputTrack_t>& currentConstraint,
    const VertexingOptions<InputTrack_t>& vertexingOptions,
    SeedFinderState_t& seedFinderState,
    const std::vector<const InputTrack_t*>& removedSeedTracks) const
    -> Result<Vertex<InputTrack_t>> {
  VertexingOptions<InputTrack_t> seedOptions = vertexingOptions;
  seedOptions.constraint = currentConstraint;

  if constexpr (NeedsRemovedTracks<typename sfinder_t::State>::value) {
    seedFinderState.tracksToRemove = removedSeedTracks;
  }

  // Run seed finder
  auto seedResult =
      m_cfg.seedFinder.find(trackVector, seedOptions, seedFinderState);

  if (!seedResult.ok()) {
    return seedResult.error();
  }

  Vertex<InputTrack_t> seedVertex = (*seedResult).back();
  // Update constraints according to seed vertex
  setConstraintAfterSeeding(currentConstraint, seedOptions.useConstraintInFit,
                            seedVertex);

  return seedVertex;
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::
    setConstraintAfterSeeding(Vertex<InputTrack_t>& currentConstraint,
                              bool useVertexConstraintInFit,
                              Vertex<InputTrack_t>& seedVertex) const -> void {
  if (useVertexConstraintInFit) {
    if (!m_cfg.useSeedConstraint) {
      // Set seed vertex constraint to old constraint before seeding
      seedVertex.setFullCovariance(currentConstraint.fullCovariance());
    } else {
      // Use the constraint provided by the seed finder
      currentConstraint.setFullPosition(seedVertex.fullPosition());
      currentConstraint.setFullCovariance(seedVertex.fullCovariance());
    }
  } else {
    currentConstraint.setFullPosition(seedVertex.fullPosition());
    currentConstraint.setFullCovariance(SquareMatrix4::Identity() *
                                        m_cfg.looseConstrValue);
    currentConstraint.setFitQuality(m_cfg.defaultConstrFitQuality);
  }
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::getIPSignificance(
    const InputTrack_t* track, const Vertex<InputTrack_t>& vtx,
    const VertexingOptions<InputTrack_t>& vertexingOptions) const
    -> Result<double> {
  // TODO: In original implementation the covariance of the given vertex is set
  // to zero. I did the same here now, but consider removing this and just
  // passing the vtx object to the estimator without changing its covariance.
  // After all, the vertex seed does have a non-zero convariance in general and
  // it probably should be used.
  Vertex<InputTrack_t> newVtx = vtx;
  if (!m_cfg.useVertexCovForIPEstimation) {
    newVtx.setFullCovariance(SquareMatrix4::Zero());
  }

  auto estRes = m_cfg.ipEstimator.getImpactParameters(
      m_extractParameters(*track), newVtx, vertexingOptions.geoContext,
      vertexingOptions.magFieldContext, m_cfg.useTime);
  if (!estRes.ok()) {
    return estRes.error();
  }

  ImpactParametersAndSigma ipas = *estRes;

  // TODO: throw error when encountering negative standard deviations
  double chi2Time = 0;
  if (m_cfg.useTime) {
    if (ipas.sigmaDeltaT.value() > 0) {
      chi2Time = std::pow(ipas.deltaT.value() / ipas.sigmaDeltaT.value(), 2);
    }
  }

  double significance = 0.;
  if (ipas.sigmaD0 > 0 && ipas.sigmaZ0 > 0) {
    significance = std::sqrt(std::pow(ipas.d0 / ipas.sigmaD0, 2) +
                             std::pow(ipas.z0 / ipas.sigmaZ0, 2) + chi2Time);
  }

  return significance;
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::
    addCompatibleTracksToVertex(
        const std::vector<const InputTrack_t*>& tracks,
        Vertex<InputTrack_t>& vtx, FitterState_t& fitterState,
        const VertexingOptions<InputTrack_t>& vertexingOptions) const
    -> Result<void> {
  for (const auto& trk : tracks) {
    auto params = m_extractParameters(*trk);
    auto pos = params.position(vertexingOptions.geoContext);
    // If track is too far away from vertex, do not consider checking the IP
    // significance
    if (m_cfg.tracksMaxZinterval < std::abs(pos[eZ] - vtx.position()[eZ])) {
      continue;
    }
    auto sigRes = getIPSignificance(trk, vtx, vertexingOptions);
    if (!sigRes.ok()) {
      return sigRes.error();
    }
    double ipSig = *sigRes;
    if (ipSig < m_cfg.tracksMaxSignificance) {
      // Create TrackAtVertex objects, unique for each (track, vertex) pair
      fitterState.tracksAtVerticesMap.emplace(std::make_pair(trk, &vtx),
                                              TrackAtVertex(params, trk));

      // Add the original track parameters to the list for vtx
      fitterState.vtxInfoMap[&vtx].trackLinks.push_back(trk);
    }
  }
  return {};
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::
    canRecoverFromNoCompatibleTracks(
        const std::vector<const InputTrack_t*>& allTracks,
        const std::vector<const InputTrack_t*>& seedTracks,
        Vertex<InputTrack_t>& vtx,
        const Vertex<InputTrack_t>& currentConstraint,
        FitterState_t& fitterState,
        const VertexingOptions<InputTrack_t>& vertexingOptions) const
    -> Result<bool> {
  // Recover from cases where no compatible tracks to vertex
  // candidate were found
  // TODO: This is for now how it's done in athena... this look a bit
  // nasty to me
  if (fitterState.vtxInfoMap[&vtx].trackLinks.empty()) {
    // Find nearest track to vertex candidate
    double smallestDeltaZ = std::numeric_limits<double>::max();
    double newZ = 0;
    bool nearTrackFound = false;
    for (const auto& trk : seedTracks) {
      auto pos =
          m_extractParameters(*trk).position(vertexingOptions.geoContext);
      auto zDistance = std::abs(pos[eZ] - vtx.position()[eZ]);
      if (zDistance < smallestDeltaZ) {
        smallestDeltaZ = zDistance;
        nearTrackFound = true;
        newZ = pos[eZ];
      }
    }
    if (nearTrackFound) {
      vtx.setFullPosition(Vector4(0., 0., newZ, 0.));

      // Update vertex info for current vertex
      fitterState.vtxInfoMap[&vtx] =
          VertexInfo<InputTrack_t>(currentConstraint, vtx.fullPosition());

      // Try to add compatible track with adapted vertex position
      auto res = addCompatibleTracksToVertex(allTracks, vtx, fitterState,
                                             vertexingOptions);
      if (!res.ok()) {
        return Result<bool>::failure(res.error());
      }

      if (fitterState.vtxInfoMap[&vtx].trackLinks.empty()) {
        ACTS_DEBUG(
            "No tracks near seed were found, while at least one was "
            "expected. Break.");
        return Result<bool>::success(false);
      }

    } else {
      ACTS_DEBUG("No nearest track to seed found. Break.");
      return Result<bool>::success(false);
    }
  }

  return Result<bool>::success(true);
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::
    canPrepareVertexForFit(
        const std::vector<const InputTrack_t*>& allTracks,
        const std::vector<const InputTrack_t*>& seedTracks,
        Vertex<InputTrack_t>& vtx,
        const Vertex<InputTrack_t>& currentConstraint,
        FitterState_t& fitterState,
        const VertexingOptions<InputTrack_t>& vertexingOptions) const
    -> Result<bool> {
  // Add vertex info to fitter state
  fitterState.vtxInfoMap[&vtx] =
      VertexInfo<InputTrack_t>(currentConstraint, vtx.fullPosition());

  // Add all compatible tracks to vertex
  auto resComp = addCompatibleTracksToVertex(allTracks, vtx, fitterState,
                                             vertexingOptions);
  if (!resComp.ok()) {
    return Result<bool>::failure(resComp.error());
  }

  // Try to recover from cases where adding compatible track was not possible
  auto resRec = canRecoverFromNoCompatibleTracks(allTracks, seedTracks, vtx,
                                                 currentConstraint, fitterState,
                                                 vertexingOptions);
  if (!resRec.ok()) {
    return Result<bool>::failure(resRec.error());
  }

  return Result<bool>::success(*resRec);
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::
    checkVertexAndCompatibleTracks(
        Vertex<InputTrack_t>& vtx,
        const std::vector<const InputTrack_t*>& seedTracks,
        FitterState_t& fitterState, bool useVertexConstraintInFit) const
    -> std::pair<int, bool> {
  bool isGoodVertex = false;
  int nCompatibleTracks = 0;
  for (const auto& trk : fitterState.vtxInfoMap[&vtx].trackLinks) {
    const auto& trkAtVtx =
        fitterState.tracksAtVerticesMap.at(std::make_pair(trk, &vtx));
    if ((trkAtVtx.vertexCompatibility < m_cfg.maxVertexChi2 &&
         m_cfg.useFastCompatibility) ||
        (trkAtVtx.trackWeight > m_cfg.minWeight &&
         trkAtVtx.chi2Track < m_cfg.maxVertexChi2 &&
         !m_cfg.useFastCompatibility)) {
      // TODO: Understand why looking for compatible tracks only in seed tracks
      // and not also in all tracks
      auto foundIter =
          std::find_if(seedTracks.begin(), seedTracks.end(),
                       [&trk](auto seedTrk) { return trk == seedTrk; });
      if (foundIter != seedTracks.end()) {
        nCompatibleTracks++;
        ACTS_DEBUG("Compatible track found.");

        if (m_cfg.addSingleTrackVertices && useVertexConstraintInFit) {
          isGoodVertex = true;
          break;
        }
        if (nCompatibleTracks > 1) {
          isGoodVertex = true;
          break;
        }
      }
    }
  }  // end loop over all tracks at vertex

  return {nCompatibleTracks, isGoodVertex};
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::
    removeCompatibleTracksFromSeedTracks(
        Vertex<InputTrack_t>& vtx, std::vector<const InputTrack_t*>& seedTracks,
        FitterState_t& fitterState,
        std::vector<const InputTrack_t*>& removedSeedTracks) const -> void {
  for (const auto& trk : fitterState.vtxInfoMap[&vtx].trackLinks) {
    const auto& trkAtVtx =
        fitterState.tracksAtVerticesMap.at(std::make_pair(trk, &vtx));
    if ((trkAtVtx.vertexCompatibility < m_cfg.maxVertexChi2 &&
         m_cfg.useFastCompatibility) ||
        (trkAtVtx.trackWeight > m_cfg.minWeight &&
         trkAtVtx.chi2Track < m_cfg.maxVertexChi2 &&
         !m_cfg.useFastCompatibility)) {
      // Find and remove track from seedTracks
      auto foundSeedIter =
          std::find_if(seedTracks.begin(), seedTracks.end(),
                       [&trk](auto seedTrk) { return trk == seedTrk; });
      if (foundSeedIter != seedTracks.end()) {
        seedTracks.erase(foundSeedIter);
        removedSeedTracks.push_back(trk);
      }
    }
  }
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::
    removeTrackIfIncompatible(
        Vertex<InputTrack_t>& vtx, std::vector<const InputTrack_t*>& seedTracks,
        FitterState_t& fitterState,
        std::vector<const InputTrack_t*>& removedSeedTracks,
        const GeometryContext& geoCtx) const -> bool {
  // Try to find the track with highest compatibility
  double maxCompatibility = 0;

  auto maxCompSeedIt = seedTracks.end();
  const InputTrack_t* removedTrack = nullptr;
  for (const auto& trk : fitterState.vtxInfoMap[&vtx].trackLinks) {
    const auto& trkAtVtx =
        fitterState.tracksAtVerticesMap.at(std::make_pair(trk, &vtx));
    double compatibility = trkAtVtx.vertexCompatibility;
    if (compatibility > maxCompatibility) {
      // Try to find track in seed tracks
      auto foundSeedIter =
          std::find_if(seedTracks.begin(), seedTracks.end(),
                       [&trk](auto seedTrk) { return trk == seedTrk; });
      if (foundSeedIter != seedTracks.end()) {
        maxCompatibility = compatibility;
        maxCompSeedIt = foundSeedIter;
        removedTrack = trk;
      }
    }
  }
  if (maxCompSeedIt != seedTracks.end()) {
    // Remove track with highest compatibility from seed tracks
    seedTracks.erase(maxCompSeedIt);
    removedSeedTracks.push_back(removedTrack);
  } else {
    // Could not find any seed with compatibility > 0, use alternative
    // method to remove a track from seed tracks: Closest track in z to
    // vtx candidate
    double smallestDeltaZ = std::numeric_limits<double>::max();
    auto smallestDzSeedIter = seedTracks.end();
    for (unsigned int i = 0; i < seedTracks.size(); i++) {
      auto pos = m_extractParameters(*seedTracks[i]).position(geoCtx);
      double zDistance = std::abs(pos[eZ] - vtx.position()[eZ]);
      if (zDistance < smallestDeltaZ) {
        smallestDeltaZ = zDistance;
        smallestDzSeedIter = seedTracks.begin() + i;
        removedTrack = seedTracks[i];
      }
    }
    if (smallestDzSeedIter != seedTracks.end()) {
      seedTracks.erase(smallestDzSeedIter);
      removedSeedTracks.push_back(removedTrack);
    } else {
      ACTS_DEBUG("No track found to remove. Stop vertex finding now.");
      return false;
    }
  }
  return true;
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::keepNewVertex(
    Vertex<InputTrack_t>& vtx,
    const std::vector<Vertex<InputTrack_t>*>& allVertices,
    FitterState_t& fitterState) const -> bool {
  double contamination = 0.;
  double contaminationNum = 0;
  double contaminationDeNom = 0;
  for (const auto& trk : fitterState.vtxInfoMap[&vtx].trackLinks) {
    const auto& trkAtVtx =
        fitterState.tracksAtVerticesMap.at(std::make_pair(trk, &vtx));
    double trackWeight = trkAtVtx.trackWeight;
    contaminationNum += trackWeight * (1. - trackWeight);
    contaminationDeNom += trackWeight * trackWeight;
  }
  if (contaminationDeNom != 0) {
    contamination = contaminationNum / contaminationDeNom;
  }
  if (contamination > m_cfg.maximumVertexContamination) {
    return false;
  }

  if (isMergedVertex(vtx, allVertices)) {
    return false;
  }

  return true;
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::isMergedVertex(
    const Vertex<InputTrack_t>& vtx,
    const std::vector<Vertex<InputTrack_t>*>& allVertices) const -> bool {
  const Vector4& candidatePos = vtx.fullPosition();
  const SquareMatrix4& candidateCov = vtx.fullCovariance();

  for (const auto otherVtx : allVertices) {
    if (&vtx == otherVtx) {
      continue;
    }
    const Vector4& otherPos = otherVtx->fullPosition();
    const SquareMatrix4& otherCov = otherVtx->fullCovariance();

    double significance = 0;
    if (!m_cfg.do3dSplitting) {
      const double deltaZPos = otherPos[eZ] - candidatePos[eZ];
      const double sumVarZ = otherCov(eZ, eZ) + candidateCov(eZ, eZ);
      if (sumVarZ <= 0) {
        // TODO FIXME this should never happen
        continue;
      }
      // Use only z significance
      significance = std::abs(deltaZPos) / std::sqrt(sumVarZ);
    } else {
      if (m_cfg.useTime) {
        // Use 4D information for significance
        const Vector4 deltaPos = otherPos - candidatePos;
        SquareMatrix4 sumCov = candidateCov + otherCov;
        auto sumCovInverse = safeInverse(sumCov);
        if (!sumCovInverse) {
          // TODO FIXME this should never happen
          continue;
        }
        significance = std::sqrt(deltaPos.dot(*sumCovInverse * deltaPos));
      } else {
        // Use 3D information for significance
        const Vector3 deltaPos = otherPos.head<3>() - candidatePos.head<3>();
        SquareMatrix3 sumCov =
            candidateCov.topLeftCorner<3, 3>() + otherCov.topLeftCorner<3, 3>();
        auto sumCovInverse = safeInverse(sumCov);
        if (!sumCovInverse) {
          // TODO FIXME this should never happen
          continue;
        }
        significance = std::sqrt(deltaPos.dot(*sumCovInverse * deltaPos));
      }
    }
    if (significance < m_cfg.maxMergeVertexSignificance) {
      return true;
    }
  }
  return false;
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::deleteLastVertex(
    Vertex<InputTrack_t>& vtx,
    std::vector<std::unique_ptr<Vertex<InputTrack_t>>>& allVertices,
    std::vector<Vertex<InputTrack_t>*>& allVerticesPtr,
    FitterState_t& fitterState,
    const VertexingOptions<InputTrack_t>& vertexingOptions) const
    -> Result<void> {
  allVertices.pop_back();
  allVerticesPtr.pop_back();

  // Update fitter state with removed vertex candidate
  fitterState.removeVertexFromMultiMap(vtx);
  // fitterState.vertexCollection contains all vertices that will be fit. When
  // we called addVtxToFit, vtx and all vertices that share tracks with vtx were
  // added to vertexCollection. Now, we want to refit the same set of vertices
  // excluding vx. Therefore, we remove vtx from vertexCollection.
  auto removeResult = fitterState.removeVertexFromCollection(vtx, logger());
  if (!removeResult.ok()) {
    return removeResult.error();
  }

  for (auto& entry : fitterState.tracksAtVerticesMap) {
    // Delete all linearized tracks for current (bad) vertex
    if (entry.first.second == &vtx) {
      entry.second.isLinearized = false;
    }
  }

  // If no vertices share tracks with vtx we don't need to refit
  if (fitterState.vertexCollection.empty()) {
    return {};
  }

  // Do the fit with removed vertex
  auto fitResult =
      m_cfg.vertexFitter.fit(fitterState, m_cfg.linearizer, vertexingOptions);
  if (!fitResult.ok()) {
    return fitResult.error();
  }

  return {};
}

template <typename vfitter_t, typename sfinder_t>
auto Acts::AdaptiveMultiVertexFinder<vfitter_t, sfinder_t>::getVertexOutputList(
    const std::vector<Vertex<InputTrack_t>*>& allVerticesPtr,
    FitterState_t& fitterState) const
    -> Acts::Result<std::vector<Vertex<InputTrack_t>>> {
  std::vector<Vertex<InputTrack_t>> outputVec;
  for (auto vtx : allVerticesPtr) {
    auto& outVtx = *vtx;
    std::vector<TrackAtVertex<InputTrack_t>> tracksAtVtx;
    for (const auto& trk : fitterState.vtxInfoMap[vtx].trackLinks) {
      tracksAtVtx.push_back(
          fitterState.tracksAtVerticesMap.at(std::make_pair(trk, vtx)));
    }
    outVtx.setTracksAtVertex(tracksAtVtx);
    outputVec.push_back(outVtx);
  }
  return Result<std::vector<Vertex<InputTrack_t>>>(outputVec);
}
