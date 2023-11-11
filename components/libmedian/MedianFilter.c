/*
 * MedianFilter.c
 *
 *  Created on: May 19, 2018
 *      Author: alexandru.bogdan
 *      Editor: Carlos Derseher
 *
 *      original source code:
 *      https://github.com/accabog/MedianFilter
 */

/**
 * This Module expects odd numbers of buffer lengths!!!
 */

#include <stdint.h>
#include "esp_log.h"
#include "MedianFilter.h"
static const char *TAG = "MEDIAN";
/**
 *
 */
int MEDIANFILTER_Init(sMedianFilter_t *medianFilter) {
  if (medianFilter && medianFilter->medianBuffer &&
      (medianFilter->numNodes % 2) && (medianFilter->numNodes > 1)) {
    // initialize buffer nodes
    for (unsigned int i = 0; i < medianFilter->numNodes; i++) {
      medianFilter->medianBuffer[i].value = INT64_MAX;
      medianFilter->medianBuffer[i].nextAge =
          &medianFilter->medianBuffer[(i + 1) % medianFilter->numNodes];
      medianFilter->medianBuffer[i].nextValue =
          &medianFilter->medianBuffer[(i + 1) % medianFilter->numNodes];
      medianFilter->medianBuffer[(i + 1) % medianFilter->numNodes].prevValue =
          &medianFilter->medianBuffer[i];
    }
    // initialize heads
    medianFilter->ageHead = medianFilter->medianBuffer;
    medianFilter->valueHead = medianFilter->medianBuffer;
    medianFilter->medianHead = medianFilter->medianBuffer;

    medianFilter->bufferCnt = 0;

    return 0;
  }

  return -1;
}

/**
 *
 */
int64_t MEDIANFILTER_Insert(sMedianFilter_t *medianFilter, int64_t sample) {
  unsigned int i;
  sMedianNode_t *newNode, *it;
  //ESP_LOGI(TAG, "old: cnt: %d, sample: %lld, agehead: %lld, valuehead: %lld, medianhead: %lld", medianFilter->bufferCnt, sample, medianFilter->ageHead->value, medianFilter->valueHead->value, medianFilter->medianHead->value);
  if (medianFilter->bufferCnt < medianFilter->numNodes) {
    medianFilter->bufferCnt++;
  }

  // if oldest node is also the smallest node,
  // increment value head
  if (medianFilter->ageHead == medianFilter->valueHead) {
    medianFilter->valueHead = medianFilter->valueHead->nextValue;
  }

  if (((medianFilter->ageHead == medianFilter->medianHead) ||
      (medianFilter->ageHead->value > medianFilter->medianHead->value)) &&
      (medianFilter->bufferCnt >= medianFilter->numNodes)) {
    // prepare for median correction
    medianFilter->medianHead = medianFilter->medianHead->prevValue;
    //ESP_LOGI(TAG, "shift left 1");
  }

  // replace age head with new sample
  newNode = medianFilter->ageHead;
  newNode->value = sample;

  // remove age head from list
  medianFilter->ageHead->nextValue->prevValue =
      medianFilter->ageHead->prevValue;
  medianFilter->ageHead->prevValue->nextValue =
      medianFilter->ageHead->nextValue;
  // increment age head
  medianFilter->ageHead = medianFilter->ageHead->nextAge;

  // find new node position
  it = medianFilter->valueHead;  // set iterator as value head
  for (i = 0; i < medianFilter->bufferCnt - 1; i++) {
    if (sample < it->value) {
      break;
    }
    it = it->nextValue;
  }
  if (i == 0) {  // replace value head if new node is the smallest
    medianFilter->valueHead = newNode;
  }
  // insert new node in list
  it->prevValue->nextValue = newNode;
  newNode->prevValue = it->prevValue;
  it->prevValue = newNode;
  newNode->nextValue = it;

  // adjust median node
  if ((medianFilter->bufferCnt < medianFilter->numNodes)){
      if (medianFilter->bufferCnt % 2 != 0 && medianFilter->bufferCnt != 1) {
          medianFilter->medianHead = medianFilter->medianHead->prevValue;
          //ESP_LOGI(TAG, "shift left 2");
      }
      if (((i > (medianFilter->bufferCnt / 2)) && (medianFilter->bufferCnt % 2 != 0)) ||
        ((i >= (medianFilter->bufferCnt / 2)) && (medianFilter->bufferCnt % 2 == 0))) {
        medianFilter->medianHead = medianFilter->medianHead->nextValue;
        //ESP_LOGI(TAG, "shift right 1");
      }
  }
  else if (i >= (medianFilter->bufferCnt / 2) ) {
    medianFilter->medianHead = medianFilter->medianHead->nextValue;
    //ESP_LOGI(TAG, "shift right 2");
  }
    //ESP_LOGI(TAG, "new: cnt: %d, i: %d, agehead: %lld, valuehead: %lld, medianhead: %lld", medianFilter->bufferCnt, i, medianFilter->ageHead->value, medianFilter->valueHead->value, medianFilter->medianHead->value);
  
  return medianFilter->medianHead->value;
}

/**
 *
 */
int64_t MEDIANFILTER_get_median(sMedianFilter_t *medianFilter, uint32_t n) {
  int64_t avgMedian = 0;
  sMedianNode_t *it;
  int32_t i;

  if ((n % 2) == 0) {
    it = medianFilter->medianHead
             ->prevValue;  // set iterator as value head previous
    // first add previous values
    for (i = 0; i < n / 2; i++) {
      avgMedian += it->value;
      it = medianFilter->medianHead->prevValue;
    }

    it =
        medianFilter->medianHead->nextValue;  // set iterator as value head next
    // second add next values
    for (i = 0; i < n / 2; i++) {
      avgMedian += it->value;
      it = medianFilter->medianHead->nextValue;
    }
  }

  avgMedian += medianFilter->medianHead->value;

  if (n > 0) {
    avgMedian /= (n + 1);
  }

  return avgMedian;
}

/**
 *
 */
uint32_t MEDIANFILTER_isFull(sMedianFilter_t *medianFilter, uint32_t n) {
  if (n < 1 || n > medianFilter->numNodes) {
      n = medianFilter->numNodes;
  }
  if (medianFilter->bufferCnt >= n) {
    return 1;
  } else {
    return 0;
  }
}

void MEDIANFILTER_log(sMedianFilter_t *medianFilter) {
    ESP_LOGI(TAG, "cnt: %d, agehead: %lld, valuehead: %lld, medianhead: %lld", medianFilter->bufferCnt, medianFilter->ageHead->value, medianFilter->valueHead->value, medianFilter->medianHead->value);
    sMedianNode_t *it = medianFilter->valueHead;
    for (int i=0; i < medianFilter->numNodes;i++) {
        if (it==medianFilter->ageHead) {
            ESP_LOGI(TAG, "%d: sample: %lld, agehead", i, it->value);
        } else if (it==medianFilter->valueHead) {
            ESP_LOGI(TAG, "%d: sample: %lld, valuehead", i, it->value);
        } else if (it==medianFilter->medianHead) {
            ESP_LOGI(TAG, "%d:sample: %lld, medianhead", i, it->value);
        } else {
            ESP_LOGI(TAG, "%d: sample: %lld", i, it->value);}
        it = it->nextValue;
    }
}
