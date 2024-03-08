const flowChartPluginContainer = document.querySelectorAll('.in2tumflowchart_show');
if (flowChartPluginContainer.length > 0) {
    import('./libs/flowChart.js').then(({default: flowChart}) => {
        flowChart(flowChartPluginContainer);
    });
}
